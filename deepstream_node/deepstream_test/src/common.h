/* Helpers shared by the pipeline prototypes. */
#ifndef COMMON_H
#define COMMON_H
#include <gst/gst.h>

/* Create an element and put it in the pipeline right away, so the pipeline
 * owns it and a single unref of the pipeline frees everything. */
static inline GstElement *make_and_add(GstElement *pipeline, const gchar *factory, const gchar *name)
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
#endif
