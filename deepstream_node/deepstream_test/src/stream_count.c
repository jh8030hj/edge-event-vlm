/*
 * Stream from the IMX219 CSI camera, count frames, and after N seconds stop
 * and save the next frame as a JPEG (headless, no display).
 * The stop condition (elapsed time) is where a detection event goes later.
 *
 * Pipeline:
 *   nvarguscamerasrc -> capsfilter(NVMM NV12, WxH@FPS)
 *     -> nvvideoconvert -> capsfilter(NVMM I420) -> nvjpegenc -> filesink
 *
 * Build: make   (from deepstream_test/)
 * Run:   ./bin/stream_count [seconds] [output.jpg]   (default 5, output/stream_last.jpg)
 * Exit:  0 if a JPEG was written, 1 otherwise
 */
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include "common.h"

#define WIDTH  1280
#define HEIGHT 720
#define FPS    60
/* White balance: 1 auto (colour varies run to run), 3 fluorescent, 5 daylight, 9 manual */
#ifndef WBMODE
#define WBMODE 3
#endif

#define ENC_CAPS "video/x-raw(memory:NVMM), format=I420"
#define MAX_SECONDS 3600

typedef struct {
  GMainLoop  *loop;
  GstElement *eos_target; /* element whose sink pad gets the EOS */
  guint       seconds;
  gint        frames;       /* buffers seen so far, written only by the probe */
  guint       saved_frames; /* frames seen when the saved one passed (later ones are dropped) */
  gint        stop;         /* set by the timer: the next frame is the one to save */
  gint        saved;        /* set once the frame to save has passed the probe */
  gint64      first_us, last_us;
  gboolean    success;
} AppData;

static gboolean on_timeout(gpointer data)
{
  g_atomic_int_set(&((AppData *)data)->stop, 1);
  return G_SOURCE_REMOVE;
}

/* Live counter on one terminal line, overwritten by the final "Done" line. */
static gboolean show_count(gpointer data)
{
  AppData *app = (AppData *)data;

  g_print("\r  %.1fs / %us  %d frames", (g_get_monotonic_time() - app->first_us) / 1e6,
          app->seconds, g_atomic_int_get(&app->frames));
  fflush(stdout);
  return G_SOURCE_CONTINUE;
}

/* The N seconds count from the first frame, not from pipeline start-up. */
static gboolean start_timer(gpointer data)
{
  AppData *app = (AppData *)data;
  g_timeout_add(app->seconds * 1000, on_timeout, app);
  g_timeout_add(200, show_count, app);
  return G_SOURCE_REMOVE;
}

/* Push EOS in from the main thread, not from the streaming thread. It goes to
 * the first element after the source: sending it to the pipeline/source makes
 * nvarguscamerasrc block. The probe drops later buffers, so nothing follows it. */
static gboolean send_eos(gpointer data)
{
  GstPad *pad = gst_element_get_static_pad(((AppData *)data)->eos_target, "sink");
  gst_pad_send_event(pad, gst_event_new_eos());
  gst_object_unref(pad);
  return G_SOURCE_REMOVE;
}

/* Drop every buffer except the first one after the timer fired, so filesink
 * receives exactly one frame. */
static GstPadProbeReturn probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  AppData *app = (AppData *)data;

  if (g_atomic_int_add(&app->frames, 1) == 0) {
    app->first_us = g_get_monotonic_time();
    g_idle_add(start_timer, app);
  }
  if (!g_atomic_int_get(&app->stop) || !g_atomic_int_compare_and_exchange(&app->saved, 0, 1))
    return GST_PAD_PROBE_DROP;

  app->saved_frames = g_atomic_int_get(&app->frames);
  app->last_us = g_get_monotonic_time();
  g_idle_add(send_eos, app);
  return GST_PAD_PROBE_OK;
}

/* Ctrl-C: leave through cleanup so the half-written JPEG is removed. */
static gboolean on_sigint(gpointer data)
{
  g_print("\n"); /* end the counter line */
  g_main_loop_quit(((AppData *)data)->loop);
  return G_SOURCE_REMOVE;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
  AppData *app = (AppData *)data;

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      if (g_atomic_int_get(&app->saved)) {
        gdouble secs = (app->last_us - app->first_us) / 1e6;
        g_print("\rDone: %u frames in %.2fs (%.1f fps)\n", app->saved_frames, secs,
                secs > 0 ? (app->saved_frames - 1) / secs : 0.0);
        app->success = TRUE;
      } else {
        g_printerr("\nStream ended before the stop time, no image saved\n");
      }
      g_main_loop_quit(app->loop);
      break;
    case GST_MESSAGE_ERROR: {
      gchar *debug = NULL;
      GError *error = NULL;
      gst_message_parse_error(msg, &error, &debug);
      g_printerr("\nERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
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

int main(int argc, char *argv[])
{
  const gchar *out;
  gboolean started = FALSE; /* filesink may have touched 'out' */
  AppData app = { 0 };
  GstElement *pipeline = NULL;
  GstElement *src, *camfilter, *conv, *encfilter, *enc, *sink;
  GstCaps *caps;
  gchar *caps_str, *end = NULL;
  GstBus *bus;
  GstPad *srcpad;
  guint bus_watch_id = 0;
  long seconds = 5;

  gst_init(&argc, &argv); /* strips --gst-* args, so read ours after it */
  if (argc > 1) {
    seconds = strtol(argv[1], &end, 10);
    if (*end != '\0' || seconds < 1 || seconds > MAX_SECONDS) {
      g_printerr("Usage: %s [seconds 1-%d] [output.jpg]\n", argv[0], MAX_SECONDS);
      return 1;
    }
  }
  out = (argc > 2) ? argv[2] : "output/stream_last.jpg";
  app.seconds = (guint)seconds;
  app.loop = g_main_loop_new(NULL, FALSE);

  pipeline = gst_pipeline_new("stream-pipeline");
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
  app.eos_target = camfilter;

  g_object_set(G_OBJECT(src), "wbmode", WBMODE, NULL);

  /* Fix the capture size; this also selects the sensor mode */
  caps_str = g_strdup_printf(
      "video/x-raw(memory:NVMM), format=NV12, width=%d, height=%d, framerate=%d/1",
      WIDTH, HEIGHT, FPS);
  caps = gst_caps_from_string(caps_str);
  g_free(caps_str);
  g_object_set(G_OBJECT(camfilter), "caps", caps, NULL);
  gst_caps_unref(caps);

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
  gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, probe, &app, NULL);
  gst_object_unref(srcpad);

  g_unix_signal_add(SIGINT, on_sigint, &app);
  g_print("Streaming %dx%d@%d for %lds -> %s\n", WIDTH, HEIGHT, FPS, seconds, out);
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
