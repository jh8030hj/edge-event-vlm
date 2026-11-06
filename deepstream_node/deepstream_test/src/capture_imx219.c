/*
 * Capture one JPEG frame from the IMX219 CSI camera (headless, no display).
 * Structure follows DeepStream sample deepstream-test1 (bus_call + main loop).
 *
 * Pipeline:
 *   nvarguscamerasrc -> capsfilter(NVMM NV12, WxH@FPS)
 *     -> nvvideoconvert -> capsfilter(NVMM I420) -> nvjpegenc -> filesink
 *
 * Build: make   (from deepstream_test/)
 * Run:   ./bin/capture_imx219 [output.jpg]   (default output/imx219.jpg)
 * Exit:  0 if a JPEG was written, 1 otherwise
 */
#include <gst/gst.h>
#include <glib/gstdio.h>

#define NUM_BUFFERS 60 /* skip the first frames: auto exposure needs time to settle */

/* Capture size. Must match one of the sensor modes that nvarguscamerasrc
 * prints at startup ("GST_ARGUS: Available Sensor modes"). */
#define WIDTH  1280
#define HEIGHT 720
#define FPS    60

/* Keep the frame in NVMM (GPU-accessible) memory all the way to the encoder.
 * If linking fails on your JetPack version, remove "(memory:NVMM)" here. */
/* White balance: 1 auto (colour varies run to run), 3 fluorescent, 5 daylight, 9 manual */
#ifndef WBMODE
#define WBMODE 3
#endif

#define ENC_CAPS "video/x-raw(memory:NVMM), format=I420"

typedef struct {
  GMainLoop *loop;
  guint      frame_count; /* buffers seen by drop_probe */
  gboolean   success;     /* TRUE only if the last frame reached the file */
} AppData;

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
  AppData *app = (AppData *)data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      if (app->frame_count >= NUM_BUFFERS) {
        g_print("Done\n");
        app->success = TRUE;
      } else {
        g_printerr("Stream ended after %u frames, no image saved\n", app->frame_count);
      }
      g_main_loop_quit(app->loop);
      break;
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

/* Drop all but the last buffer so filesink receives exactly one frame. */
static GstPadProbeReturn drop_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  AppData *app = (AppData *)data;
  app->frame_count++;
  return (app->frame_count < NUM_BUFFERS) ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

/* Create an element and put it in the pipeline right away, so the pipeline
 * owns it and a single unref of the pipeline frees everything. */
static GstElement *make_and_add(GstElement *pipeline, const gchar *factory, const gchar *name)
{
  GstElement *e = gst_element_factory_make(factory, name);
  if (!e) {
    g_printerr("Failed to create '%s' (is the %s plugin installed?)\n", name, factory);
    return NULL;
  }
  if (!gst_bin_add(GST_BIN(pipeline), e)) {
    g_printerr("Failed to add '%s' to the pipeline\n", name);
    return NULL;
  }
  return e;
}

int main(int argc, char *argv[])
{
  const gchar *out;
  gboolean started = FALSE; /* filesink may have touched 'out' */
  AppData app = { NULL, 0, FALSE };
  GstElement *pipeline = NULL;
  GstElement *src, *camfilter, *conv, *encfilter, *enc, *sink;
  GstCaps *caps;
  gchar *caps_str;
  GstBus *bus;
  GstPad *srcpad;
  guint bus_watch_id = 0;

  gst_init(&argc, &argv);
  out = (argc > 1) ? argv[1] : "output/imx219.jpg"; /* after gst_init: it strips --gst-* args */
  app.loop = g_main_loop_new(NULL, FALSE);

  pipeline = gst_pipeline_new("capture-pipeline");
  if (!pipeline) {
    g_printerr("Failed to create pipeline\n");
    goto cleanup;
  }

  src       = make_and_add(pipeline, "nvarguscamerasrc", "src");
  camfilter = make_and_add(pipeline, "capsfilter",       "camfilter");
  conv      = make_and_add(pipeline, "nvvideoconvert",   "conv");
  encfilter = make_and_add(pipeline, "capsfilter",       "encfilter");
  enc       = make_and_add(pipeline, "nvjpegenc",        "enc");
  sink      = make_and_add(pipeline, "filesink",         "sink");
  if (!src || !camfilter || !conv || !encfilter || !enc || !sink)
    goto cleanup;

  /* Camera: stop after NUM_BUFFERS frames */
  g_object_set(G_OBJECT(src), "num-buffers", NUM_BUFFERS, "wbmode", WBMODE, NULL);

  /* Fix the capture size so the output resolution is predictable */
  caps_str = g_strdup_printf(
      "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d, framerate=%d/1",
      WIDTH, HEIGHT, FPS);
  caps = gst_caps_from_string(caps_str);
  g_free(caps_str);
  g_object_set(G_OBJECT(camfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  /* Encoder input format */
  caps = gst_caps_from_string(ENC_CAPS);
  g_object_set(G_OBJECT(encfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

  g_object_set(G_OBJECT(sink), "location", out, NULL);

  if (!gst_element_link_many(src, camfilter, conv, encfilter, enc, sink, NULL)) {
    g_printerr("Failed to link elements (caps mismatch?)\n");
    goto cleanup;
  }

  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_watch_id = gst_bus_add_watch(bus, bus_call, &app);
  gst_object_unref(bus);

  srcpad = gst_element_get_static_pad(src, "src");
  gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, drop_probe, &app, NULL);
  gst_object_unref(srcpad);

  g_print("Capturing %dx%d -> %s\n", WIDTH, HEIGHT, out);
  started = TRUE;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Failed to start pipeline (camera connected? nvargus-daemon running?)\n");
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
  if (started && !app.success)
    g_remove(out); /* filesink creates the file at start; don't leave an empty/partial JPEG */
  return app.success ? 0 : 1;
}
