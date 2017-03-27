/*
 * Copyright (C) 2014 SUMOMO Computer Association
 *     Author: ayaka <ayaka@soulik.info>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "gstv4l2videoenc.h"
#include "v4l2_calls.h"

#include <string.h>
#include <gst/gst-i18n-plugin.h>

GST_DEBUG_CATEGORY_STATIC (gst_v4l2_video_enc_debug);
#define GST_CAT_DEFAULT gst_v4l2_video_enc_debug

#define MAX_CODEC_FRAME (2 * 1024 * 1024)

static gboolean gst_v4l2_video_enc_flush (GstVideoEncoder * encoder);

enum
{
  PROP_0,
  V4L2_STD_OBJECT_PROPS,
};

#define gst_v4l2_video_enc_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE (GstV4l2VideoEnc, gst_v4l2_video_enc,
    GST_TYPE_VIDEO_ENCODER);

static void
gst_v4l2_video_enc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (object);

  switch (prop_id) {
      /* Split IO mode so output is configure through 'io-mode' and capture
       * through 'capture-io-mode' */
    case PROP_IO_MODE:
      gst_v4l2_object_set_property_helper (self->v4l2output,
          PROP_IO_MODE, value, pspec);
      break;
    case PROP_CAPTURE_IO_MODE:
      gst_v4l2_object_set_property_helper (self->v4l2capture,
          prop_id, value, pspec);
      break;

    case PROP_DEVICE:
      gst_v4l2_object_set_property_helper (self->v4l2output,
          prop_id, value, pspec);
      gst_v4l2_object_set_property_helper (self->v4l2capture,
          prop_id, value, pspec);
      break;
    case PROP_EXTRA_CONTROLS:
      gst_v4l2_object_set_property_helper (self->v4l2output,
          prop_id, value, pspec);
      break;

      /* By default, only set on output */
    default:
      if (!gst_v4l2_object_set_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_v4l2_video_enc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (object);

  switch (prop_id) {
    case PROP_IO_MODE:
      gst_v4l2_object_get_property_helper (self->v4l2output,
          prop_id, value, pspec);
      break;
    case PROP_CAPTURE_IO_MODE:
      gst_v4l2_object_get_property_helper (self->v4l2output,
          PROP_IO_MODE, value, pspec);
      break;

    case PROP_EXTRA_CONTROLS:
      gst_v4l2_object_get_property_helper (self->v4l2output,
          prop_id, value, pspec);
      break;
      /* By default read from output */
    default:
      if (!gst_v4l2_object_get_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static gboolean
gst_v4l2_video_enc_open (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Opening");

  if (!gst_v4l2_object_open (self->v4l2output))
    goto failure;

  if (!gst_v4l2_object_open_shared (self->v4l2capture, self->v4l2output))
    goto failure;

  self->probed_sinkcaps = gst_v4l2_object_get_caps (self->v4l2output,
      gst_v4l2_object_get_raw_caps ());

  if (gst_caps_is_empty (self->probed_sinkcaps))
    goto no_raw_format;

  self->probed_srccaps = gst_v4l2_object_get_caps (self->v4l2capture,
      gst_v4l2_object_get_codec_caps ());

  if (gst_caps_is_empty (self->probed_srccaps))
    goto no_encoded_format;

  return TRUE;

no_encoded_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported output format"),
          self->v4l2output->videodev), (NULL));
  goto failure;


no_raw_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported input format"),
          self->v4l2output->videodev), (NULL));
  goto failure;

failure:
  if (GST_V4L2_IS_OPEN (self->v4l2output))
    gst_v4l2_object_close (self->v4l2output);

  if (GST_V4L2_IS_OPEN (self->v4l2capture))
    gst_v4l2_object_close (self->v4l2capture);

  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return FALSE;
}

static gboolean
gst_v4l2_video_enc_close (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Closing");

  gst_v4l2_object_close (self->v4l2output);
  gst_v4l2_object_close (self->v4l2capture);
  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return TRUE;
}

static gboolean
gst_v4l2_video_enc_start (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Starting");

  gst_v4l2_object_unlock (self->v4l2output);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_v4l2_video_enc_stop (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  gst_v4l2_object_unlock (self->v4l2output);
  gst_v4l2_object_unlock (self->v4l2capture);

  /* Wait for capture thread to stop */
  gst_pad_stop_task (encoder->srcpad);

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);
  g_assert (g_atomic_int_get (&self->processing) == FALSE);

  gst_v4l2_object_stop (self->v4l2output);
  gst_v4l2_object_stop (self->v4l2capture);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_v4l2_video_enc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  gboolean ret = TRUE;
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    if (gst_v4l2_object_caps_equal (self->v4l2output, state->caps)) {
      GST_DEBUG_OBJECT (self, "Compatible caps");
      goto done;
    }
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    /* FIXME we probably need to do more work if pools are active */
  }

  ret = gst_v4l2_object_set_format (self->v4l2output, state->caps);

  if (ret)
    self->input_state = gst_video_codec_state_ref (state);

  GST_DEBUG_OBJECT (self, "output caps: %" GST_PTR_FORMAT, state->caps);

done:
  return ret;
}

static gboolean
gst_v4l2_video_enc_flush (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  GST_DEBUG_OBJECT (self, "Flushing");

  /* Ensure the processing thread has stopped for the reverse playback
   * iscount case */
  if (g_atomic_int_get (&self->processing)) {
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

    gst_v4l2_object_unlock_stop (self->v4l2output);
    gst_v4l2_object_unlock_stop (self->v4l2capture);
    gst_pad_stop_task (encoder->srcpad);

    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

  }

  self->output_flow = GST_FLOW_OK;

  gst_v4l2_object_unlock_stop (self->v4l2output);
  gst_v4l2_object_unlock_stop (self->v4l2capture);

  return TRUE;
}

static gboolean
gst_v4l2_video_enc_negotiate (GstVideoEncoder * encoder)
{
  return GST_VIDEO_ENCODER_CLASS (parent_class)->negotiate (encoder);
}

static GstFlowReturn
gst_v4l2_video_enc_finish (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buffer;

  if (!g_atomic_int_get (&self->processing))
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing encoding");

  /* Keep queuing empty buffers until the processing thread has stopped,
   * _pool_process() will return FLUSHING when that happened */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  while (ret == GST_FLOW_OK) {
    buffer = gst_buffer_new ();
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL
        (self->v4l2output->pool), &buffer);
    gst_buffer_unref (buffer);
  }

  /* and ensure the processing thread has stopped in case another error
   * occured. */
  gst_v4l2_object_unlock (self->v4l2capture);
  gst_pad_stop_task (encoder->srcpad);
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (encoder, "Done draining buffers");

done:
  return ret;
}

static GstVideoCodecFrame *
gst_v4l2_video_enc_get_oldest_frame (GstVideoEncoder * encoder)
{
  GstVideoCodecFrame *frame = NULL;
  GList *frames, *l;
  gint count = 0;

  frames = gst_video_encoder_get_frames (encoder);

  for (l = frames; l != NULL; l = l->next) {
    GstVideoCodecFrame *f = l->data;

    if (!frame || frame->pts > f->pts)
      frame = f;

    count++;
  }

  if (frame) {
    GST_LOG_OBJECT (encoder,
        "Oldest frame is %d %" GST_TIME_FORMAT
        " and %d frames left",
        frame->system_frame_number, GST_TIME_ARGS (frame->pts), count - 1);
    gst_video_codec_frame_ref (frame);
  }

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  return frame;
}

static void
gst_v4l2_video_enc_loop (GstVideoEncoder * encoder)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  GstVideoCodecFrame *frame;
  GstBuffer *buffer = NULL;
  GstFlowReturn ret;

  GST_LOG_OBJECT (encoder, "Allocate output buffer");

  buffer = gst_video_encoder_allocate_output_buffer (encoder, MAX_CODEC_FRAME);

  if (NULL == buffer) {
    ret = GST_FLOW_FLUSHING;
    goto beach;
  }


  /* FIXME Check if buffer isn't the last one here */

  GST_LOG_OBJECT (encoder, "Process output buffer");
  ret =
      gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL
      (self->v4l2capture->pool), &buffer);

  if (ret != GST_FLOW_OK)
    goto beach;

  frame = gst_v4l2_video_enc_get_oldest_frame (encoder);

  if (frame) {
    frame->output_buffer = buffer;
    buffer = NULL;
    ret = gst_video_encoder_finish_frame (encoder, frame);

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (encoder, "Encoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

beach:
  GST_DEBUG_OBJECT (encoder, "Leaving output thread");

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  g_atomic_int_set (&self->processing, FALSE);
  gst_v4l2_object_unlock (self->v4l2output);
  gst_pad_pause_task (encoder->srcpad);
}

static void
gst_v4l2_video_enc_loop_stopped (GstV4l2VideoEnc * self)
{
  if (g_atomic_int_get (&self->processing)) {
    GST_DEBUG_OBJECT (self, "Early stop of encoding thread");
    self->output_flow = GST_FLOW_FLUSHING;
    g_atomic_int_set (&self->processing, FALSE);
  }

  GST_DEBUG_OBJECT (self, "Encoding task destroyed: %s",
      gst_flow_get_name (self->output_flow));

}

static GstFlowReturn
gst_v4l2_video_enc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame, GstCaps * outcaps)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2output))) {
    if (!self->input_state)
      goto not_negotiated;
    if (!gst_v4l2_object_set_format (self->v4l2output, self->input_state->caps))
      goto not_negotiated;
  }

  if (NULL != outcaps) {
    GstBufferPool *pool = GST_BUFFER_POOL (self->v4l2output->pool);
    gst_v4l2_object_set_format (self->v4l2capture, outcaps);

    if (!gst_buffer_pool_is_active (pool)) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
      gint min = self->v4l2output->min_buffers == 0 ? GST_V4L2_MIN_BUFFERS :
              self->v4l2output->min_buffers;

      gst_buffer_pool_config_set_params (config,
          self->input_state->caps, self->v4l2output->info.size, min, min);

      if (!gst_buffer_pool_set_config (pool, config))
        goto activate_failed;

      if (!gst_buffer_pool_set_active (pool, TRUE))
        goto activate_failed;
    }

    gst_video_encoder_set_output_state (encoder, outcaps, self->input_state);

    if (!gst_video_encoder_negotiate (encoder)) {
      if (GST_PAD_IS_FLUSHING (encoder->srcpad))
        goto flushing;
      else
        goto not_negotiated;
    }

    if (!gst_buffer_pool_set_active
        (GST_BUFFER_POOL (self->v4l2capture->pool), TRUE)) {
      GST_DEBUG ("active capture pool failed");
      goto activate_failed;
    }
  }

  if (g_atomic_int_get (&self->processing) == FALSE) {
    /* It possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK &&
        self->output_flow != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

    GST_DEBUG_OBJECT (self, "Starting encoding thread");

    /* Start the processing task, when it quits, the task will disable input
     * processing to unlock input if draining, or prevent potential block */
    g_atomic_int_set (&self->processing, TRUE);
    if (!gst_pad_start_task (encoder->srcpad,
            (GstTaskFunction) gst_v4l2_video_enc_loop, self,
            (GDestroyNotify) gst_v4l2_video_enc_loop_stopped))
      goto start_task_failed;
  }

  if (frame->input_buffer) {
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL
        (self->v4l2output->pool), &frame->input_buffer);
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

    if (ret == GST_FLOW_FLUSHING) {
      if (g_atomic_int_get (&self->processing) == FALSE)
        ret = self->output_flow;
      goto drop;
    } else if (ret != GST_FLOW_OK) {
      goto process_failed;
    }

    /* No need to keep input arround */
    gst_buffer_replace (&frame->input_buffer, NULL);
  }

  gst_video_codec_frame_unref (frame);
  return ret;
  /* ERRORS */
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto drop;
  }
activate_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        (_("Failed to allocate required memory.")),
        ("Buffer pool activation failed"));
    return GST_FLOW_ERROR;

  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }
start_task_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to start encoding thread.")), (NULL));
    g_atomic_int_set (&self->processing, FALSE);
    ret = GST_FLOW_ERROR;
    goto drop;
  }
process_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to process frame.")),
        ("Maybe be due to not enough memory or failing driver"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
drop:
  {
    gst_video_encoder_finish_frame (encoder, frame);
    return ret;
  }
}

static gboolean
gst_v4l2_video_enc_decide_allocation (GstVideoEncoder *
    encoder, GstQuery * query)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  GstClockTime latency;
  gboolean ret = FALSE;

  self->v4l2capture->info.size = MAX_CODEC_FRAME;

  if (gst_v4l2_object_decide_allocation (self->v4l2capture, query))
    ret =
        GST_VIDEO_ENCODER_CLASS
        (parent_class)->decide_allocation (encoder, query);
  latency = self->v4l2capture->min_buffers * self->v4l2capture->duration;
  gst_video_encoder_set_latency (encoder, latency, latency);
  return ret;
}

static gboolean
gst_v4l2_video_enc_propose_allocation (GstVideoEncoder *
    encoder, GstQuery * query)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (self, "called");

  if (query == NULL)
    ret = TRUE;
  else
    ret = gst_v4l2_object_propose_allocation (self->v4l2output, query);

  if (ret)
    ret = GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (encoder,
        query);

  return ret;
}

static gboolean
gst_v4l2_video_enc_src_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      GstPad *pad = GST_VIDEO_ENCODER_SRC_PAD (encoder);

      gst_query_parse_caps (query, &filter);

      if (self->probed_srccaps)
        result = gst_caps_ref (self->probed_srccaps);
      else
        result = gst_pad_get_pad_template_caps (pad);

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning src caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->src_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_v4l2_video_enc_sink_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      GstPad *pad = GST_VIDEO_ENCODER_SINK_PAD (encoder);

      gst_query_parse_caps (query, &filter);

      if (self->probed_sinkcaps)
        result = gst_caps_ref (self->probed_sinkcaps);
      else
        result = gst_pad_get_pad_template_caps (pad);

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning sink caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_v4l2_video_enc_sink_event (GstVideoEncoder * encoder, GstEvent * event)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (encoder);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_v4l2_object_unlock (self->v4l2output);
      gst_v4l2_object_unlock (self->v4l2capture);
      break;
    default:
      break;
  }

  ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_event (encoder, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_pad_stop_task (encoder->srcpad);
      GST_DEBUG_OBJECT (self, "flush start done");
    default:
      break;
  }

  return ret;
}

static GstStateChangeReturn
gst_v4l2_video_enc_change_state (GstElement * element,
    GstStateChange transition)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (element);
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
    gst_pad_stop_task (encoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_v4l2_video_enc_dispose (GObject * object)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (object);

  gst_caps_replace (&self->probed_sinkcaps, NULL);
  gst_caps_replace (&self->probed_srccaps, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_v4l2_video_enc_finalize (GObject * object)
{
  GstV4l2VideoEnc *self = GST_V4L2_VIDEO_ENC (object);

  gst_v4l2_object_destroy (self->v4l2capture);
  gst_v4l2_object_destroy (self->v4l2output);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_v4l2_video_enc_init (GstV4l2VideoEnc * self)
{
  /* V4L2 object are created in subinstance_init */
}

static void
gst_v4l2_video_enc_class_init (GstV4l2VideoEncClass * klass)
{
  GstElementClass *element_class;
  GObjectClass *gobject_class;
  GstVideoEncoderClass *video_encoder_class;

  parent_class = g_type_class_peek_parent (klass);

  element_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;
  video_encoder_class = (GstVideoEncoderClass *) klass;

  GST_DEBUG_CATEGORY_INIT (gst_v4l2_video_enc_debug, "v4l2videoenc", 0,
      "V4L2 Video Encoder");

  gst_element_class_set_static_metadata (element_class,
      "V4L2 Video Encoder",
      "Codec/Encoder/Video",
      "Encode video streams via V4L2 API", "ayaka <ayaka@soulik.info>");

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_finalize);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_get_property);

  video_encoder_class->open = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_open);
  video_encoder_class->close = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_close);
  video_encoder_class->start = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_start);
  video_encoder_class->stop = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_stop);
  video_encoder_class->finish = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_finish);
  video_encoder_class->flush = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_flush);
  video_encoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_set_format);
  video_encoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_negotiate);
  video_encoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_decide_allocation);
  video_encoder_class->propose_allocation =
       GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_propose_allocation);
  video_encoder_class->sink_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_sink_query);
  video_encoder_class->src_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_src_query);
  video_encoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_sink_event);


  klass->handle_frame = GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_handle_frame);

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_enc_change_state);

  gst_v4l2_object_install_m2m_properties_helper (gobject_class);
}

/* Probing functions */
gboolean
gst_v4l2_is_video_enc (GstCaps * sink_caps, GstCaps * src_caps)
{
  gboolean ret = FALSE;

  if (gst_caps_is_subset (sink_caps, gst_v4l2_object_get_raw_caps ())
      && gst_caps_is_subset (src_caps, gst_v4l2_object_get_codec_caps ()))
    ret = TRUE;

  return ret;
}
