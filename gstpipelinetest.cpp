// Compile with:
// gcc -o gstpipelinetest gstpipelinetest.cpp -I /usr/include/glib-2.0/ -I /usr/include/gstreamer-1.0/ -I /usr/lib64/glib-2.0/include -lglib-2.0 -lgobject-2.0 -lgstreamer-1.0

#include <gst/gst.h>

static const char alsasink_device_[] = "hw:0,0";
static const char playbin_uri_[] = "file:///mnt/data/Music/FLAC/Albums/Crosby,_Stills_&_Nash/Daylight_Again/01_-_Crosby,_Stills_&_Nash_-_Daylight_Again_-_Turn_Your_Back_On_Love.flac";

typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *audiobin;
  GstElement *queue;
  GstElement *convert;
  GstElement *tee;
  GstElement *audioqueue;
  GstElement *probequeue;
  GstElement *audioconverter;
  GstElement *probeconverter;
  GstElement *audiosink;
  GstElement *probesink;
  int audio_buffer_received;
  int probe_buffer_received;
} CustomData;

static void pad_added_handler(GstElement *src, GstPad *pad, CustomData *data);
static GstPadProbeReturn probe_buffer_cb(GstPad *pad, GstPadProbeInfo *info, CustomData *data);
static GstPadProbeReturn audio_buffer_cb(GstPad *pad, GstPadProbeInfo *info, CustomData *data);

int main(int argc, char *argv[]) {

  CustomData data;

  // Initialize GStreamer
  gst_init(&argc, &argv);

  // Create the playbin pipeline
  data.pipeline = gst_element_factory_make("playbin", "pipeline");
  if (!data.pipeline) {
    g_printerr("Playbin could not be created.\n");
    return -1;
  }

  // Set the URI
  g_object_set(data.pipeline, "uri", playbin_uri_, nullptr);

  // Connect to the pad-added signal
  g_signal_connect(data.pipeline, "pad-added", G_CALLBACK(pad_added_handler), &data);

  // Create audio bin
  data.audiobin = gst_bin_new("audiobin");
  if (!data.audiobin) {
    g_printerr("Audiobin could not be created.\n");
    return -1;
  }

  // Create fake sink
  data.probesink = gst_element_factory_make("fakesink", "fakesink");
  g_assert(data.probesink);
  gst_bin_add(GST_BIN(data.audiobin), data.probesink);
  g_object_set(G_OBJECT(data.probesink), "sync", TRUE, nullptr);

  // Create audio sink
  data.audiosink = gst_element_factory_make("alsasink", "alsa");
  g_assert(data.audiosink);
  gst_bin_add(GST_BIN(data.audiobin), data.audiosink);

  // Set output device
  g_object_set(G_OBJECT(data.audiosink), "device", alsasink_device_, nullptr);

  // Create the other elements

  data.queue = gst_element_factory_make("queue2", "queue");
  data.convert = gst_element_factory_make("audioconvert", "convert");
  data.tee = gst_element_factory_make("tee", "tee");
  data.audioqueue = gst_element_factory_make("queue2", "audioqueue");
  data.probequeue = gst_element_factory_make("queue2", "probequeue");
  data.audioconverter = gst_element_factory_make("audioconvert", "audioconverter");
  data.probeconverter = gst_element_factory_make("audioconvert", "probeconverter");
  data.audio_buffer_received = 0;
  data.probe_buffer_received = 0;

  if (!data.probesink ||
      !data.audiosink ||
      !data.queue ||
      !data.convert ||
      !data.tee ||
      !data.audioqueue ||
      !data.probequeue ||
      !data.audioconverter ||
      !data.probeconverter) {
    g_printerr("Not all elements could be created.\n");
    return -1;
  }

  gst_bin_add_many(GST_BIN(data.audiobin), data.queue, data.convert, data.tee, data.audioqueue, data.probequeue, data.audioconverter, data.probeconverter, nullptr);

  // Create a pad on the outside of the audiobin and connect it to the pad of the queue element.
  {
    GstPad *pad = gst_element_get_static_pad(data.queue, "sink");
    gst_element_add_pad(data.audiobin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(pad);
  }

  // Link: queue --> convert ---> tee
  gst_element_link_many(data.queue, data.convert, data.tee, nullptr);

  // Link the outputs of tee to the queues on each path:
  // tee --> probequeue + audioqueue

  {
    GstPad *pad = gst_element_get_static_pad(data.audioqueue, "sink");
    gst_pad_link(gst_element_get_request_pad(data.tee, "src_%u"), pad);
    gst_object_unref(pad);
  }

  {
    GstPad *pad = gst_element_get_static_pad(data.probequeue, "sink");
    gst_pad_link(gst_element_get_request_pad(data.tee, "src_%u"), pad);
    gst_object_unref(pad);
  }

  // Let the audio output of the tee, autonegotiate the format:
  // audioqueue --> audioconverter
  {
    GstCaps *caps = gst_caps_new_empty_simple("audio/x-raw");
    //GstCaps *caps = gst_caps_new_any();
    gst_element_link_filtered(data.audioqueue, data.audioconverter, caps);
    gst_caps_unref(caps);
  }

  // Link the probe queue of the tee and force 16 bit caps:
  // probequeue --> probeconverter
  {
    GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", nullptr);
    //GstCaps *caps = gst_caps_new_empty_simple("audio/x-raw");
    gst_element_link_filtered(data.probequeue, data.probeconverter, caps);
    gst_caps_unref(caps);
  }

  // Link the queues to the sinks:
  // probeconverter --> probesink
  // audioconverter --> audiosink
  gst_element_link(data.probeconverter, data.probesink);
  gst_element_link(data.audioconverter, data.audiosink);

  // Add probes
  {
    GstPad *pad = gst_element_get_static_pad(data.probequeue, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback) probe_buffer_cb, &data, nullptr);
    gst_object_unref(pad);
  }
  {
    GstPad *pad = gst_element_get_static_pad(data.audioqueue, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback) audio_buffer_cb, &data, nullptr);
    gst_object_unref(pad);
  }

  // Set playbin's sink to be our costum audio-sink.
  g_object_set(GST_OBJECT(data.pipeline), "audio-sink", data.audiobin, nullptr);

  // Start playing
  GstStateChangeReturn ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data.pipeline);
    return -1;
  }

  /* Listen to the bus */
  gboolean terminate = FALSE;
  GstBus *bus = gst_element_get_bus(data.pipeline);
  do {
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ANY);

    /* Parse message */
    if (msg) {
      GError *err;
      gchar *debug_info;
      switch(GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
          gst_message_parse_error(msg, &err, &debug_info);
          g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
          g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
          g_clear_error(&err);
          g_free(debug_info);
          terminate = TRUE;
          break;
        case GST_MESSAGE_EOS:
          g_print("End-Of-Stream reached.\n");
          terminate = TRUE;
          break;
        case GST_MESSAGE_STATE_CHANGED:
          /* We are only interested in state-changed messages from the pipeline */
          if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data.pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            g_print("Pipeline state changed from %s to %s:\n", gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
          }
          break;
        default:
          break;
      }
      gst_message_unref(msg);
    }
  }
  while (!terminate);

  /* Free resources */
  gst_object_unref(bus);
  gst_element_set_state(data.pipeline, GST_STATE_NULL);
  gst_object_unref(data.pipeline);

  return 0;

}

static void pad_added_handler(GstElement *src, GstPad *new_pad, CustomData *data) {

  g_assert(src);
  g_assert(new_pad);
  g_assert(data);

  GstPad *sink_pad = gst_element_get_static_pad(data->audiobin, "sink");
  g_assert(sink_pad);

  g_print ("Received new pad '%s' from '%s':\n", GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

  // If our converter is already linked, we have nothing to do here
  if (gst_pad_is_linked(sink_pad)) {
    g_print ("We are already linked. Ignoring.\n");
    gst_object_unref(sink_pad);
    return;
  }

  // Check the new pad's type
  GstCaps *new_pad_caps = gst_pad_get_current_caps(new_pad);
  g_assert(new_pad_caps);

  GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
  g_assert(new_pad_struct);

  const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);
  if (!g_str_has_prefix(new_pad_type, "audio/x-raw")) {
    g_print ("It has type '%s' which is not raw audio. Ignoring.\n", new_pad_type);
    if (new_pad_caps) gst_caps_unref (new_pad_caps);
    gst_object_unref(sink_pad);
    return;
  }
  g_print("pad: %s\n", new_pad_type);

  // Attempt the link
  GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
  if (GST_PAD_LINK_FAILED(ret)) {
    g_print("Type is '%s' but link failed.\n", new_pad_type);
  }
  else {
    g_print("Link succeeded (type '%s').\n", new_pad_type);
  }

}

static GstPadProbeReturn probe_buffer_cb(GstPad *pad, GstPadProbeInfo *info, CustomData *data) {

  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstStructure *structure = gst_caps_get_structure(caps, 0);
  const gchar *format = gst_structure_get_string(structure, "format");

  ++data->probe_buffer_received;
  if (data->probe_buffer_received >= 40) data->probe_buffer_received = 1;

  if (data->probe_buffer_received == 1) {
    data->probe_buffer_received = true;
    g_print("Probe buffer is %s\n", format);
  }

  return GST_PAD_PROBE_OK;

}


static GstPadProbeReturn audio_buffer_cb(GstPad *pad, GstPadProbeInfo *info, CustomData *data) {

  GstCaps *caps = gst_pad_get_current_caps(pad);
  GstStructure *structure = gst_caps_get_structure(caps, 0);
  const gchar *format = gst_structure_get_string(structure, "format");

  ++data->audio_buffer_received;
  if (data->audio_buffer_received >= 40) data->audio_buffer_received = 1;

  if (data->audio_buffer_received == 1) {
    data->audio_buffer_received = true;
    g_print("Audio buffer is %s\n", format);
  }

  return GST_PAD_PROBE_OK;

}
