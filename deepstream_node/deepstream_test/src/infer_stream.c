/*
 * Stream from the IMX219 CSI camera through a TensorRT classifier (nvinfer)
 * and print the top class about once a second (headless, console only).
 *
 * Pipeline:
 *   nvarguscamerasrc -> capsfilter(NVMM NV12, WxH@FPS)
 *     -> nvvideoconvert(center crop) -> capsfilter(NVMM NV12, NET_SIZE^2)
 *     -> nvstreammux -> nvinfer(config) -> fakesink
 *
 * The center crop mirrors torchvision Resize(256)+CenterCrop(224): the middle
 * 87.5% of the frame height, as a square, scaled to the network size.
 *
 * Build: make   (from deepstream_test/)
 * Run:   ./bin/infer_stream [seconds] [config]   (default 10, config/model.txt)
 * Exit:  0 if frames were classified, 1 otherwise
 */
#include <gst/gst.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include "gstnvdsmeta.h"
#include "common.h"

#define WIDTH  1280
#define HEIGHT 720
#define FPS    60
/* White balance: 1 auto (colour varies run to run), 3 fluorescent, 5 daylight, 9 manual */
#ifndef WBMODE
#define WBMODE 3
#endif

#define NET_SIZE    224                  /* must match infer-dims in the config */
#define CROP_SIZE   (HEIGHT * 7 / 8)     /* 630 */
#define MAX_SECONDS 3600

typedef struct {
  GMainLoop  *loop;
  GstElement *eos_target; /* element whose sink pad gets the EOS */
  guint       seconds;
  gint        frames;     /* written only by the probe */
  gint        printed_frames;
  gint64      first_us, last_us, printed_us;
  gboolean    success;
} AppData;

/* Push EOS in from the main thread to the first element after the source:
 * sending it to the pipeline/source makes nvarguscamerasrc block. */
static gboolean on_timeout(gpointer data)
{
  GstPad *pad = gst_element_get_static_pad(((AppData *)data)->eos_target, "sink");
  gst_pad_send_event(pad, gst_event_new_eos());
  gst_object_unref(pad);
  return G_SOURCE_REMOVE;
}

/* The N seconds count from the first frame, not from pipeline start-up. */
static gboolean start_timer(gpointer data)
{
  g_timeout_add(((AppData *)data)->seconds * 1000, on_timeout, data);
  return G_SOURCE_REMOVE;
}

/* Print the top class of the first frame in each 1s window. A full-frame
 * classifier hangs its result on a frame-sized object meta. */
static GstPadProbeReturn probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  AppData *app = (AppData *)data;
  gint64 now = g_get_monotonic_time();
  NvDsBatchMeta *batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
  gint n = g_atomic_int_add(&app->frames, 1) + 1;

  if (n == 1) {
    app->first_us = now;
    g_idle_add(start_timer, app);
  }
  app->last_us = now;
  if (!batch || now - app->printed_us < G_USEC_PER_SEC)
    return GST_PAD_PROBE_OK;
  gint64 since = app->printed_us ? app->printed_us : app->first_us;
  gdouble fps = now > since ? (n - app->printed_frames) * 1e6 / (now - since) : 0;
  app->printed_us = now;
  app->printed_frames = n;

  const gchar *label = "(below threshold)";
  gfloat prob = 0;
  for (GList *f = batch->frame_meta_list; f; f = f->next)
    for (GList *o = ((NvDsFrameMeta *)f->data)->obj_meta_list; o; o = o->next)
      for (GList *c = ((NvDsObjectMeta *)o->data)->classifier_meta_list; c; c = c->next)
        for (GList *l = ((NvDsClassifierMeta *)c->data)->label_info_list; l; l = l->next) {
          NvDsLabelInfo *li = (NvDsLabelInfo *)l->data;
          if (li->result_prob >= prob) {
            label = li->result_label;
            prob = li->result_prob;
          }
        }
  g_print("frame %d: %s (%.1f%%) %.1f fps\n", n, label, prob * 100, fps);
  return GST_PAD_PROBE_OK;
}

/* Ctrl-C: leave through cleanup. */
static gboolean on_sigint(gpointer data)
{
  AppData *app = (AppData *)data;
  g_print("\n");
  app->success = g_atomic_int_get(&app->frames) > 1;
  g_main_loop_quit(app->loop);
  return G_SOURCE_REMOVE;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
  AppData *app = (AppData *)data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS: {
      gint frames = g_atomic_int_get(&app->frames);
      gdouble secs = (app->last_us - app->first_us) / 1e6;
      if (frames > 1 && secs > 0) {
        g_print("Done: %d frames in %.2fs (%.1f fps)\n", frames, secs, (frames - 1) / secs);
        app->success = TRUE;
      } else {
        g_printerr("Stream ended before any frame was classified\n");
      }
      g_main_loop_quit(app->loop);
      break;
    }
    case GST_MESSAGE_ERROR: {
      gchar *debug = NULL;
      GError *error = NULL;
      gst_message_parse_error(msg, &error, &debug);
      g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
      if (debug)
        g_printerr("Debug info: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      g_main_loop_quit(app->loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static void set_caps(GstElement *filter, const gchar *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  gchar *s = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  GstCaps *caps = gst_caps_from_string(s);
  g_object_set(G_OBJECT(filter), "caps", caps, NULL);
  gst_caps_unref(caps);
  g_free(s);
}

int main(int argc, char *argv[])
{
  const gchar *config;
  AppData app = { 0 };
  GstElement *pipeline = NULL;
  GstElement *src, *camfilter, *conv, *netfilter, *mux, *infer, *sink;
  GstPad *pad, *sinkpad;
  GstBus *bus;
  guint bus_watch_id = 0;
  gchar *crop, *end = NULL;
  long seconds = 10;

  gst_init(&argc, &argv); /* strips --gst-* args, so read ours after it */
  if (argc > 1) {
    seconds = strtol(argv[1], &end, 10);
    if (*end != '\0' || seconds < 1 || seconds > MAX_SECONDS) {
      g_printerr("Usage: %s [seconds 1-%d] [config]\n", argv[0], MAX_SECONDS);
      return 1;
    }
  }
  config = (argc > 2) ? argv[2] : "config/model.txt";
  if (!g_file_test(config, G_FILE_TEST_IS_REGULAR)) {
    g_printerr("Config not found: %s\n", config);
    return 1;
  }
  app.seconds = (guint)seconds;
  app.loop = g_main_loop_new(NULL, FALSE);

  pipeline = gst_pipeline_new("infer-pipeline");
  if (!pipeline) {
    g_printerr("Failed to create pipeline\n");
    goto cleanup;
  }
  src       = make_and_add(pipeline, "nvarguscamerasrc", "src");
  camfilter = make_and_add(pipeline, "capsfilter",       "camfilter");
  conv      = make_and_add(pipeline, "nvvideoconvert",   "conv");
  netfilter = make_and_add(pipeline, "capsfilter",       "netfilter");
  mux       = make_and_add(pipeline, "nvstreammux",      "mux");
  infer     = make_and_add(pipeline, "nvinfer",          "infer");
  sink      = make_and_add(pipeline, "fakesink",         "sink");
  if (!src || !camfilter || !conv || !netfilter || !mux || !infer || !sink)
    goto cleanup;
  app.eos_target = camfilter;

  g_object_set(src, "wbmode", WBMODE, NULL);
  set_caps(camfilter, "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d, framerate=%d/1",
           WIDTH, HEIGHT, FPS);
  crop = g_strdup_printf("%d:%d:%d:%d", (WIDTH - CROP_SIZE) / 2, (HEIGHT - CROP_SIZE) / 2,
                         CROP_SIZE, CROP_SIZE);
  g_object_set(conv, "src-crop", crop, NULL);
  g_free(crop);
  set_caps(netfilter, "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d", NET_SIZE, NET_SIZE);
  g_object_set(mux, "batch-size", 1, "width", NET_SIZE, "height", NET_SIZE, "live-source", TRUE, NULL);
  g_object_set(infer, "config-file-path", config, NULL);
  g_object_set(sink, "sync", FALSE, NULL);

  if (!gst_element_link_many(src, camfilter, conv, netfilter, NULL)) {
    g_printerr("Failed to link the camera branch (caps mismatch?)\n");
    goto cleanup;
  }
  pad = gst_element_get_static_pad(netfilter, "src");
  sinkpad = gst_element_request_pad_simple(mux, "sink_0");
  gboolean linked = sinkpad && gst_pad_link(pad, sinkpad) == GST_PAD_LINK_OK;
  gst_object_unref(pad);
  if (sinkpad)
    gst_object_unref(sinkpad);
  if (!linked) {
    g_printerr("Failed to link netfilter to nvstreammux\n");
    goto cleanup;
  }
  if (!gst_element_link_many(mux, infer, sink, NULL)) {
    g_printerr("Failed to link nvstreammux -> nvinfer -> sink\n");
    goto cleanup;
  }

  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_watch_id = gst_bus_add_watch(bus, bus_call, &app);
  gst_object_unref(bus);

  pad = gst_element_get_static_pad(infer, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, probe, &app, NULL);
  gst_object_unref(pad);

  g_unix_signal_add(SIGINT, on_sigint, &app);
  g_print("Classifying %dx%d@%d for %lds with %s\n", WIDTH, HEIGHT, FPS, seconds, config);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to start pipeline (engine/config valid? camera connected?)\n");
    goto cleanup;
  }
  g_main_loop_run(app.loop);

cleanup:
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  if (bus_watch_id)
    g_source_remove(bus_watch_id);
  if (app.loop)
    g_main_loop_unref(app.loop);
  return app.success ? 0 : 1;
}
