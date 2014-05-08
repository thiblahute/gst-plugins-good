/* Generic video mixer plugin
 * Copyright (C) 2004, 2008 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
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
 */

/**
 * SECTION:gstbasemixer
 * @short_description: Base class for video mixers
 *
 * Basemixer can accept AYUV, ARGB and BGRA video streams. For each of the requested
 * sink pads it will compare the incoming geometry and framerate to define the
 * output parameters. Indeed output video frames will have the geometry of the
 * biggest incoming video stream and the framerate of the fastest incoming one.
 *
 * Basemixer will do colorspace conversion.
 * 
 * Zorder for each input stream can be configured on the
 * #GstBasemixerPad.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstbasemixer.h"
#include "gstbasemixerpad.h"
#include "videoconvert.h"

GST_DEBUG_CATEGORY_STATIC (gst_basemixer_debug);
#define GST_CAT_DEFAULT gst_basemixer_debug

#define GST_BASE_MIXER_GET_LOCK(mix) \
  (&GST_BASE_MIXER(mix)->lock)
#define GST_BASE_MIXER_LOCK(mix) \
  (g_mutex_lock(GST_BASE_MIXER_GET_LOCK (mix)))
#define GST_BASE_MIXER_UNLOCK(mix) \
  (g_mutex_unlock(GST_BASE_MIXER_GET_LOCK (mix)))
#define GST_BASE_MIXER_GET_SETCAPS_LOCK(mix) \
  (&GST_BASE_MIXER(mix)->setcaps_lock)
#define GST_BASE_MIXER_SETCAPS_LOCK(mix) \
  (g_mutex_lock(GST_BASE_MIXER_GET_SETCAPS_LOCK (mix)))
#define GST_BASE_MIXER_SETCAPS_UNLOCK(mix) \
  (g_mutex_unlock(GST_BASE_MIXER_GET_SETCAPS_LOCK (mix)))

#define FORMATS " { AYUV, BGRA, ARGB, RGBA, ABGR, Y444, Y42B, YUY2, UYVY, "\
                "   YVYU, I420, YV12, NV12, NV21, Y41B, RGB, BGR, xRGB, xBGR, "\
                "   RGBx, BGRx } "

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS))
    );

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS))
    );

static void gst_basemixer_child_proxy_init (gpointer g_iface,
    gpointer iface_data);
static gboolean gst_basemixer_push_sink_event (GstBasemixer * mix,
    GstEvent * event);
static void gst_basemixer_release_pad (GstElement * element, GstPad * pad);
static void gst_basemixer_reset_qos (GstBasemixer * mix);

struct _GstBasemixerCollect
{
  GstBasemixerPad *mixpad;

  GstBuffer *queued;            /* buffer for which we don't know the end time yet */
  GstVideoInfo queued_vinfo;

  GstBuffer *buffer;            /* buffer that should be blended now */
  GstVideoInfo buffer_vinfo;

  GstClockTime start_time;
  GstClockTime end_time;
};

#define DEFAULT_PAD_ZORDER 0
enum
{
  PROP_PAD_0,
  PROP_PAD_ZORDER,
};

G_DEFINE_TYPE (GstBasemixerPad, gst_basemixer_pad,
    GST_TYPE_BASE_AGGREGATOR_PAD);
#define gst_basemixer_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstBasemixer, gst_basemixer,
    GST_TYPE_BASE_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_basemixer_child_proxy_init));

static gboolean gst_basemixer_src_setcaps (GstPad * pad, GstBasemixer * mix,
    GstCaps * caps);

static gboolean
gst_basemixer_update_src_caps (GstBasemixer * mix)
{
  GSList *l;
  gint best_width = -1, best_height = -1;
  gdouble best_fps = -1, cur_fps;
  gint best_fps_n = -1, best_fps_d = -1;
  gboolean ret = TRUE;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (mix);
  GstBasemixerClass *mixer_klass = (GstBasemixerClass *) klass;
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR (mix);

  GST_BASE_MIXER_SETCAPS_LOCK (mix);
  GST_BASE_MIXER_LOCK (mix);

  for (l = mix->sinkpads; l; l = l->next) {
    GstBasemixerPad *mpad = l->data;
    gint this_width, this_height;
    gint fps_n, fps_d;
    gint width, height;

    fps_n = GST_VIDEO_INFO_FPS_N (&mpad->info);
    fps_d = GST_VIDEO_INFO_FPS_D (&mpad->info);
    width = GST_VIDEO_INFO_WIDTH (&mpad->info);
    height = GST_VIDEO_INFO_HEIGHT (&mpad->info);

    if (width == 0 || height == 0)
      continue;

    this_width = width;
    this_height = height;

    if (best_width < this_width)
      best_width = this_width;
    if (best_height < this_height)
      best_height = this_height;

    if (fps_d == 0)
      cur_fps = 0.0;
    else
      gst_util_fraction_to_double (fps_n, fps_d, &cur_fps);

    if (best_fps < cur_fps) {
      best_fps = cur_fps;
      best_fps_n = fps_n;
      best_fps_d = fps_d;
    }
  }

  if (best_fps_n <= 0 || best_fps_d <= 0 || best_fps == 0.0) {
    best_fps_n = 25;
    best_fps_d = 1;
    best_fps = 25.0;
  }

  if (best_width > 0 && best_height > 0 && best_fps > 0) {
    GstCaps *caps, *peercaps;
    GstStructure *s;
    GstVideoInfo info;

    if (GST_VIDEO_INFO_FPS_N (&mix->info) != best_fps_n ||
        GST_VIDEO_INFO_FPS_D (&mix->info) != best_fps_d) {
      if (agg->segment.position != -1) {
        mix->ts_offset = agg->segment.position - agg->segment.start;
        mix->nframes = 0;
      }
    }
    gst_video_info_init (&info);
    gst_video_info_set_format (&info, GST_VIDEO_INFO_FORMAT (&mix->info),
        best_width, best_height);
    info.fps_n = best_fps_n;
    info.fps_d = best_fps_d;
    info.par_n = GST_VIDEO_INFO_PAR_N (&mix->info);
    info.par_d = GST_VIDEO_INFO_PAR_D (&mix->info);

    if (mixer_klass->modify_src_pad_info)
      if (!mixer_klass->modify_src_pad_info (mix, &info)) {
        ret = FALSE;
        GST_BASE_MIXER_UNLOCK (mix);
        goto done;
      }

    caps = gst_video_info_to_caps (&info);

    peercaps = gst_pad_peer_query_caps (agg->srcpad, NULL);
    if (peercaps) {
      GstCaps *tmp;

      s = gst_caps_get_structure (caps, 0);
      gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT, "height",
          GST_TYPE_INT_RANGE, 1, G_MAXINT, "framerate", GST_TYPE_FRACTION_RANGE,
          0, 1, G_MAXINT, 1, NULL);

      tmp = gst_caps_intersect (caps, peercaps);
      gst_caps_unref (caps);
      gst_caps_unref (peercaps);
      caps = tmp;
      if (gst_caps_is_empty (caps)) {
        GST_DEBUG_OBJECT (mix, "empty caps");
        ret = FALSE;
        GST_BASE_MIXER_UNLOCK (mix);
        goto done;
      }

      caps = gst_caps_truncate (caps);
      s = gst_caps_get_structure (caps, 0);
      gst_structure_fixate_field_nearest_int (s, "width", info.width);
      gst_structure_fixate_field_nearest_int (s, "height", info.height);
      gst_structure_fixate_field_nearest_fraction (s, "framerate", best_fps_n,
          best_fps_d);

      gst_structure_get_int (s, "width", &info.width);
      gst_structure_get_int (s, "height", &info.height);
      gst_structure_get_fraction (s, "framerate", &info.fps_n, &info.fps_d);
    }

    gst_caps_unref (caps);
    caps = gst_video_info_to_caps (&info);

    GST_BASE_MIXER_UNLOCK (mix);
    ret = gst_basemixer_src_setcaps (agg->srcpad, mix, caps);
    gst_caps_unref (caps);
  } else {
    GST_BASE_MIXER_UNLOCK (mix);
  }

done:
  GST_BASE_MIXER_SETCAPS_UNLOCK (mix);

  return ret;
}

static gboolean
gst_basemixer_update_converters (GstBasemixer * mix)
{
  GSList *tmp;
  GstVideoFormat best_format;
  GstVideoInfo best_info;
  GstBasemixerPad *pad;
  gboolean need_alpha = FALSE;
  gboolean at_least_one_alpha = FALSE;
  GstCaps *downstream_caps;
  GstCaps *possible_caps;
  gchar *best_colorimetry;
  const gchar *best_chroma;
  GHashTable *formats_table = g_hash_table_new (g_direct_hash, g_direct_equal);
  gint best_format_number = 0;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (mix);
  GstBasemixerClass *mixer_klass = (GstBasemixerClass *) klass;
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR (mix);

  best_format = GST_VIDEO_FORMAT_UNKNOWN;
  gst_video_info_init (&best_info);

  downstream_caps = gst_pad_get_allowed_caps (agg->srcpad);

  if (mixer_klass->get_preferred_input_caps) {
    GstCaps *preferred_caps = mixer_klass->get_preferred_input_caps (mix);
    downstream_caps = gst_caps_intersect (downstream_caps, preferred_caps);
  }

  if (!downstream_caps || gst_caps_is_empty (downstream_caps))
    return FALSE;

  /* first find new preferred format */
  for (tmp = mix->sinkpads; tmp; tmp = tmp->next) {
    GstStructure *s;
    gint format_number;

    pad = tmp->data;

    if (!pad->info.finfo)
      continue;

    if (pad->info.finfo->flags & GST_VIDEO_FORMAT_FLAG_ALPHA)
      at_least_one_alpha = TRUE;

    /* If we want alpha, disregard all the other formats */
    if (need_alpha && !(pad->info.finfo->flags & GST_VIDEO_FORMAT_FLAG_ALPHA))
      continue;

    /* This can happen if we release a pad and another pad hasn't been negotiated yet */
    if (GST_VIDEO_INFO_FORMAT (&pad->info) == GST_VIDEO_FORMAT_UNKNOWN)
      continue;

    possible_caps = gst_video_info_to_caps (&pad->info);

    s = gst_caps_get_structure (possible_caps, 0);
    gst_structure_remove_fields (s, "width", "height", "framerate",
        "pixel-aspect-ratio", "interlace-mode", NULL);

    /* Can downstream accept this format ? */
    if (!gst_caps_can_intersect (downstream_caps, possible_caps)) {
      gst_caps_unref (possible_caps);
      continue;
    }

    gst_caps_unref (possible_caps);

    format_number =
        GPOINTER_TO_INT (g_hash_table_lookup (formats_table,
            GINT_TO_POINTER (GST_VIDEO_INFO_FORMAT (&pad->info))));
    format_number += 1;

    g_hash_table_replace (formats_table,
        GINT_TO_POINTER (GST_VIDEO_INFO_FORMAT (&pad->info)),
        GINT_TO_POINTER (format_number));

    /* If that pad is the first with alpha, set it as the new best format */
    if (!need_alpha && (pad->info.finfo->flags & GST_VIDEO_FORMAT_FLAG_ALPHA)) {
      need_alpha = TRUE;
      best_format = GST_VIDEO_INFO_FORMAT (&pad->info);
      best_info = pad->info;
      best_format_number = format_number;
    } else if (format_number > best_format_number) {
      best_format = GST_VIDEO_INFO_FORMAT (&pad->info);
      best_info = pad->info;
      best_format_number = format_number;
    }
  }

  g_hash_table_unref (formats_table);

  if (best_format == GST_VIDEO_FORMAT_UNKNOWN) {
    downstream_caps = gst_caps_fixate (downstream_caps);
    gst_video_info_from_caps (&best_info, downstream_caps);
    best_format = GST_VIDEO_INFO_FORMAT (&best_info);
  }

  gst_caps_unref (downstream_caps);

  if (at_least_one_alpha
      && !(best_info.finfo->flags & GST_VIDEO_FORMAT_FLAG_ALPHA)) {
    GST_ELEMENT_ERROR (mix, CORE, NEGOTIATION,
        ("At least one of the input pads contains alpha, but downstream can't support alpha."),
        ("Either convert your inputs to not contain alpha or add a videoconvert after the mixer"));
    return FALSE;
  }

  best_colorimetry = gst_video_colorimetry_to_string (&(best_info.colorimetry));
  best_chroma = gst_video_chroma_to_string (best_info.chroma_site);
  mix->info = best_info;

  GST_DEBUG_OBJECT (mix,
      "The output format will now be : %d with colorimetry : %s and chroma : %s",
      best_format, best_colorimetry, best_chroma);

  /* Then browse the sinks once more, setting or unsetting conversion if needed */
  for (tmp = mix->sinkpads; tmp; tmp = tmp->next) {
    gchar *colorimetry;
    const gchar *chroma;

    pad = tmp->data;

    if (!pad->info.finfo)
      continue;

    if (GST_VIDEO_INFO_FORMAT (&pad->info) == GST_VIDEO_FORMAT_UNKNOWN)
      continue;

    if (pad->convert)
      videoconvert_convert_free (pad->convert);

    pad->convert = NULL;

    colorimetry = gst_video_colorimetry_to_string (&(pad->info.colorimetry));
    chroma = gst_video_chroma_to_string (pad->info.chroma_site);

    if (best_format != GST_VIDEO_INFO_FORMAT (&pad->info) ||
        g_strcmp0 (colorimetry, best_colorimetry) ||
        g_strcmp0 (chroma, best_chroma)) {
      GST_DEBUG_OBJECT (pad, "This pad will be converted from %d to %d",
          GST_VIDEO_INFO_FORMAT (&pad->info),
          GST_VIDEO_INFO_FORMAT (&best_info));
      pad->convert = videoconvert_convert_new (&pad->info, &best_info);
      pad->need_conversion_update = TRUE;
      if (!pad->convert) {
        g_free (colorimetry);
        g_free (best_colorimetry);
        GST_WARNING ("No path found for conversion");
        return FALSE;
      }
    } else {
      GST_DEBUG_OBJECT (pad, "This pad will not need conversion");
    }
    g_free (colorimetry);
  }

  g_free (best_colorimetry);
  return TRUE;
}

static gboolean
gst_basemixer_pad_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstBasemixer *mix;
  GstBasemixerPad *mixpad;
  GstVideoInfo info;
  gboolean ret = FALSE;

  GST_INFO_OBJECT (pad, "Setting caps %" GST_PTR_FORMAT, caps);

  mix = GST_BASE_MIXER (parent);
  mixpad = GST_BASE_MIXER_PAD (pad);

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_DEBUG_OBJECT (pad, "Failed to parse caps");
    goto beach;
  }

  GST_BASE_MIXER_LOCK (mix);
  if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN) {
    if (GST_VIDEO_INFO_PAR_N (&mix->info) != GST_VIDEO_INFO_PAR_N (&info)
        || GST_VIDEO_INFO_PAR_D (&mix->info) != GST_VIDEO_INFO_PAR_D (&info) ||
        GST_VIDEO_INFO_INTERLACE_MODE (&mix->info) !=
        GST_VIDEO_INFO_INTERLACE_MODE (&info)) {
      GST_DEBUG_OBJECT (pad,
          "got input caps %" GST_PTR_FORMAT ", but " "current caps are %"
          GST_PTR_FORMAT, caps, mix->current_caps);
      GST_BASE_MIXER_UNLOCK (mix);
      return FALSE;
    }
  }

  mixpad->info = info;

  //GST_COLLECT_PADS_STREAM_LOCK (mix->collect);

  ret = gst_basemixer_update_converters (mix);

  GST_BASE_MIXER_UNLOCK (mix);
  if (ret)
    ret = gst_basemixer_update_src_caps (mix);
  //GST_COLLECT_PADS_STREAM_UNLOCK (mix->collect);

beach:
  return ret;
}

static GstCaps *
gst_basemixer_pad_sink_getcaps (GstPad * pad, GstBasemixer * mix,
    GstCaps * filter)
{
  GstCaps *srccaps;
  GstCaps *template_caps;
  GstCaps *filtered_caps;
  GstCaps *returned_caps;
  GstStructure *s;
  gboolean had_current_caps = TRUE;
  gint i, n;
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR (mix);

  template_caps = gst_pad_get_pad_template_caps (GST_PAD (agg->srcpad));

  srccaps = gst_pad_get_current_caps (GST_PAD (agg->srcpad));
  if (srccaps == NULL) {
    had_current_caps = FALSE;
    srccaps = template_caps;
  }

  srccaps = gst_caps_make_writable (srccaps);

  n = gst_caps_get_size (srccaps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (srccaps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);

    gst_structure_remove_fields (s, "colorimetry", "chroma-site", "format",
        NULL);
  }

  filtered_caps = srccaps;
  if (filter)
    filtered_caps = gst_caps_intersect (srccaps, filter);
  returned_caps = gst_caps_intersect (filtered_caps, template_caps);

  gst_caps_unref (srccaps);
  if (filter)
    gst_caps_unref (filtered_caps);
  if (had_current_caps)
    gst_caps_unref (template_caps);

  return returned_caps;
}

static gboolean
gst_basemixer_pad_sink_acceptcaps (GstPad * pad, GstBasemixer * mix,
    GstCaps * caps)
{
  gboolean ret;
  GstCaps *modified_caps;
  GstCaps *accepted_caps;
  GstCaps *template_caps;
  gboolean had_current_caps = TRUE;
  gint i, n;
  GstStructure *s;
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR (mix);

  GST_DEBUG_OBJECT (pad, "%" GST_PTR_FORMAT, caps);

  accepted_caps = gst_pad_get_current_caps (GST_PAD (agg->srcpad));

  template_caps = gst_pad_get_pad_template_caps (GST_PAD (agg->srcpad));

  if (accepted_caps == NULL) {
    accepted_caps = template_caps;
    had_current_caps = FALSE;
  }

  accepted_caps = gst_caps_make_writable (accepted_caps);

  GST_LOG_OBJECT (pad, "src caps %" GST_PTR_FORMAT, accepted_caps);

  n = gst_caps_get_size (accepted_caps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (accepted_caps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);

    gst_structure_remove_fields (s, "colorimetry", "chroma-site", "format",
        NULL);
  }

  modified_caps = gst_caps_intersect (accepted_caps, template_caps);

  ret = gst_caps_can_intersect (caps, accepted_caps);
  GST_DEBUG_OBJECT (pad, "%saccepted caps %" GST_PTR_FORMAT,
      (ret ? "" : "not "), caps);
  GST_DEBUG_OBJECT (pad, "acceptable caps are %" GST_PTR_FORMAT, accepted_caps);
  gst_caps_unref (accepted_caps);
  gst_caps_unref (modified_caps);
  if (had_current_caps)
    gst_caps_unref (template_caps);
  return ret;
}

static gboolean
gst_basemixer_sink_query (GstBaseAggregator * agg, GstBaseAggregatorPad * bpad,
    GstQuery * query)
{
  GstBasemixer *mix = GST_BASE_MIXER (agg);
  GstBasemixerPad *pad = GST_BASE_MIXER_PAD (bpad);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_basemixer_pad_sink_getcaps (GST_PAD (pad), mix, filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      ret = TRUE;
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;

      gst_query_parse_accept_caps (query, &caps);
      ret = gst_basemixer_pad_sink_acceptcaps (GST_PAD (pad), mix, caps);
      gst_query_set_accept_caps_result (query, ret);
      ret = TRUE;
      break;
    }
    default:
      ret =
          GST_BASE_AGGREGATOR_CLASS (parent_class)->pad_query (agg, bpad,
          query);
      break;
  }
  return ret;
}

static void
gst_basemixer_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstBasemixerPad *pad = GST_BASE_MIXER_PAD (object);

  switch (prop_id) {
    case PROP_PAD_ZORDER:
      g_value_set_uint (value, pad->zorder);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static int
pad_zorder_compare (const GstBasemixerPad * pad1, const GstBasemixerPad * pad2)
{
  return pad1->zorder - pad2->zorder;
}

static void
gst_basemixer_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBasemixerPad *pad = GST_BASE_MIXER_PAD (object);
  GstBasemixer *mix = GST_BASE_MIXER (gst_pad_get_parent (GST_PAD (pad)));

  switch (prop_id) {
    case PROP_PAD_ZORDER:
      GST_BASE_MIXER_LOCK (mix);
      pad->zorder = g_value_get_uint (value);

      mix->sinkpads = g_slist_sort (mix->sinkpads,
          (GCompareFunc) pad_zorder_compare);
      GST_BASE_MIXER_UNLOCK (mix);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  gst_object_unref (mix);
}

static void
gst_basemixer_pad_class_init (GstBasemixerPadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_basemixer_pad_set_property;
  gobject_class->get_property = gst_basemixer_pad_get_property;

  g_object_class_install_property (gobject_class, PROP_PAD_ZORDER,
      g_param_spec_uint ("zorder", "Z-Order", "Z Order of the picture",
          0, 10000, DEFAULT_PAD_ZORDER,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
}

static void
gst_basemixer_pad_init (GstBasemixerPad * mixerpad)
{
  mixerpad->zorder = DEFAULT_PAD_ZORDER;
  mixerpad->convert = NULL;
  mixerpad->need_conversion_update = FALSE;
  mixerpad->mixed_frame = NULL;
  mixerpad->converted_buffer = NULL;
}

/* GstBasemixer */

static void
gst_basemixer_update_qos (GstBasemixer * mix, gdouble proportion,
    GstClockTimeDiff diff, GstClockTime timestamp)
{
  GST_DEBUG_OBJECT (mix,
      "Updating QoS: proportion %lf, diff %s%" GST_TIME_FORMAT ", timestamp %"
      GST_TIME_FORMAT, proportion, (diff < 0) ? "-" : "",
      GST_TIME_ARGS (ABS (diff)), GST_TIME_ARGS (timestamp));

  GST_OBJECT_LOCK (mix);
  mix->proportion = proportion;
  if (G_LIKELY (timestamp != GST_CLOCK_TIME_NONE)) {
    if (G_UNLIKELY (diff > 0))
      mix->earliest_time =
          timestamp + 2 * diff + gst_util_uint64_scale_int_round (GST_SECOND,
          GST_VIDEO_INFO_FPS_D (&mix->info), GST_VIDEO_INFO_FPS_N (&mix->info));
    else
      mix->earliest_time = timestamp + diff;
  } else {
    mix->earliest_time = GST_CLOCK_TIME_NONE;
  }
  GST_OBJECT_UNLOCK (mix);
}

static void
gst_basemixer_reset_qos (GstBasemixer * mix)
{
  gst_basemixer_update_qos (mix, 0.5, 0, GST_CLOCK_TIME_NONE);
  mix->qos_processed = mix->qos_dropped = 0;
}

static void
gst_basemixer_read_qos (GstBasemixer * mix, gdouble * proportion,
    GstClockTime * time)
{
  GST_OBJECT_LOCK (mix);
  *proportion = mix->proportion;
  *time = mix->earliest_time;
  GST_OBJECT_UNLOCK (mix);
}

static void
gst_basemixer_reset (GstBasemixer * mix)
{
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR(mix);
  GSList *l;

  gst_video_info_init (&mix->info);
  mix->ts_offset = 0;
  mix->nframes = 0;

  gst_segment_init (&agg->segment, GST_FORMAT_TIME);
  agg->segment.position = -1;

  gst_basemixer_reset_qos (mix);

  for (l = mix->sinkpads; l; l = l->next) {
    GstBasemixerPad *p = l->data;

    gst_buffer_replace (&p->buffer, NULL);
    p->start_time = -1;
    p->end_time = -1;

    gst_video_info_init (&p->info);
  }

  mix->newseg_pending = TRUE;
}

/*  1 == OK
 *  0 == need more data
 * -1 == EOS
 * -2 == error
 */
static gint
gst_basemixer_fill_queues (GstBasemixer * mix,
    GstClockTime output_start_time, GstClockTime output_end_time)
{
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR(mix);
  GSList *l;
  gboolean eos = TRUE;
  gboolean need_more_data = FALSE;

  for (l = mix->sinkpads; l; l = l->next) {
    GstBasemixerPad *pad = l->data;
    GstSegment *segment;
    GstBaseAggregatorPad *bpad;
    GstBuffer *buf;
    GstVideoInfo *vinfo;
    gboolean is_eos;

    bpad = GST_BASE_AGGREGATOR_PAD (pad);
    segment = &bpad->segment;
    is_eos = bpad->eos;
    buf = gst_base_aggregator_pad_get_buffer (bpad);
    if (buf) {
      GstClockTime start_time, end_time;

      start_time = GST_BUFFER_TIMESTAMP (buf);
      if (start_time == -1) {
        gst_buffer_unref (buf);
        GST_DEBUG_OBJECT (pad, "Need timestamped buffers!");
        return -2;
      }

      vinfo = &pad->info;

      /* FIXME: Make all this work with negative rates */

      if ((pad->buffer && start_time < GST_BUFFER_TIMESTAMP (pad->buffer))
          || (pad->queued && start_time < GST_BUFFER_TIMESTAMP (pad->queued))) {
        GST_DEBUG_OBJECT (pad, "Buffer from the past, dropping");
        gst_buffer_unref (buf);
        need_more_data = TRUE;
        continue;
      }

      if (pad->queued) {
        end_time = start_time - GST_BUFFER_TIMESTAMP (pad->queued);
        start_time = GST_BUFFER_TIMESTAMP (pad->queued);
        gst_buffer_unref (buf);
        buf = gst_buffer_ref (pad->queued);
        vinfo = &pad->queued_vinfo;
      } else {
        end_time = GST_BUFFER_DURATION (buf);

        if (end_time == -1) {
          pad->queued = buf;
          gst_buffer_unref (buf);
          pad->queued_vinfo = pad->info;
          GST_DEBUG ("end time is -1 and nothing queued");
          need_more_data = TRUE;
          continue;
        }
      }

      g_assert (start_time != -1 && end_time != -1);
      end_time += start_time;   /* convert from duration to position */

      /* Check if it's inside the segment */
      if (start_time >= segment->stop || end_time < segment->start) {
        GST_DEBUG_OBJECT (pad,
            "Buffer outside the segment : segment: [%" GST_TIME_FORMAT " -- %"
            GST_TIME_FORMAT "]" " Buffer [%" GST_TIME_FORMAT " -- %"
            GST_TIME_FORMAT "]", GST_TIME_ARGS (segment->stop),
            GST_TIME_ARGS (segment->start), GST_TIME_ARGS (start_time),
            GST_TIME_ARGS (end_time));

        if (buf == pad->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&pad->queued, NULL);
        } else {
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      /* Clip to segment and convert to running time */
      start_time = MAX (start_time, segment->start);
      if (segment->stop != -1)
        end_time = MIN (end_time, segment->stop);
      start_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, start_time);
      end_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, end_time);
      g_assert (start_time != -1 && end_time != -1);

      /* Convert to the output segment rate */
      if (ABS (agg->segment.rate) != 1.0) {
        start_time *= ABS (agg->segment.rate);
        end_time *= ABS (agg->segment.rate);
      }

      if (pad->end_time != -1 && pad->end_time > end_time) {
        GST_DEBUG_OBJECT (pad, "Buffer from the past, dropping");
        if (buf == pad->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&pad->queued, NULL);
        } else {
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      if (end_time >= output_start_time && start_time < output_end_time) {
        GST_DEBUG_OBJECT (pad,
            "Taking new buffer with start time %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        gst_buffer_replace (&pad->buffer, buf);
        pad->buffer_vinfo = *vinfo;
        pad->start_time = start_time;
        pad->end_time = end_time;

        if (buf == pad->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&pad->queued, NULL);
        } else {
          gst_buffer_unref (buf);
        }
        eos = FALSE;
      } else if (start_time >= output_end_time) {
        GST_DEBUG_OBJECT (pad, "Keeping buffer until %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        eos = FALSE;
      } else {
        GST_DEBUG_OBJECT (pad, "Too old buffer -- dropping");
        if (buf == pad->queued) {
          gst_buffer_replace (&pad->queued, NULL);
        } else {
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }
    } else {
      if (pad->end_time != -1) {
        if (pad->end_time <= output_start_time) {
          gst_buffer_replace (&pad->buffer, NULL);
          pad->start_time = pad->end_time = -1;
          if (is_eos) {
            GST_DEBUG ("I just need more data");
            need_more_data = TRUE;
          }
        } else if (is_eos) {
          eos = FALSE;
        }
      }
    }
  }

  if (need_more_data)
    return 0;
  if (eos)
    return -1;

  return 1;
}

static GstFlowReturn
gst_basemixer_blend_buffers (GstBasemixer * mix,
    GstClockTime output_start_time, GstClockTime output_end_time,
    GstBuffer ** outbuf)
{
  GSList *l;
  guint outsize;
  GstVideoFrame outframe;
  static GstAllocationParams params = { 0, 15, 0, 0, };
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (mix);
  GstBasemixerClass *mixer_klass = (GstBasemixerClass *) klass;

  g_assert (mixer_klass->mix_frames != NULL);

  outsize = GST_VIDEO_INFO_SIZE (&mix->info);

  *outbuf = gst_buffer_new_allocate (NULL, outsize, &params);
  GST_BUFFER_TIMESTAMP (*outbuf) = output_start_time;
  GST_BUFFER_DURATION (*outbuf) = output_end_time - output_start_time;

  gst_video_frame_map (&outframe, &mix->info, *outbuf, GST_MAP_READWRITE);

  /* Here we convert all the frames the subclass will have to mix */
  for (l = mix->sinkpads; l; l = l->next) {
    GstBasemixerPad *pad = l->data;
    GstBaseAggregatorPad *bpad = GST_BASE_AGGREGATOR_PAD (pad);

    if (pad->buffer != NULL) {
      GstClockTime timestamp;
      gint64 stream_time;
      GstSegment *seg;
      GstVideoFrame *converted_frame = g_slice_new0 (GstVideoFrame);
      GstBuffer *converted_buf = NULL;
      GstVideoFrame *frame = g_slice_new0 (GstVideoFrame);

      seg = &bpad->segment;

      timestamp = GST_BUFFER_TIMESTAMP (pad->buffer);

      stream_time =
          gst_segment_to_stream_time (seg, GST_FORMAT_TIME, timestamp);

      /* sync object properties on stream time */
      if (GST_CLOCK_TIME_IS_VALID (stream_time))
        gst_object_sync_values (GST_OBJECT (pad), stream_time);


      gst_video_frame_map (frame, &pad->buffer_vinfo, pad->buffer,
          GST_MAP_READ);

      if (pad->convert) {
        gint converted_size;

        /* We wait until here to set the conversion infos, in case mix->info changed */
        if (pad->need_conversion_update) {
          pad->conversion_info = mix->info;
          gst_video_info_set_format (&(pad->conversion_info),
              GST_VIDEO_INFO_FORMAT (&mix->info), pad->info.width,
              pad->info.height);
          pad->need_conversion_update = FALSE;
        }

        converted_size = pad->conversion_info.size;
        converted_size = converted_size > outsize ? converted_size : outsize;
        converted_buf = gst_buffer_new_allocate (NULL, converted_size, &params);

        gst_video_frame_map (converted_frame, &(pad->conversion_info),
            converted_buf, GST_MAP_READWRITE);
        videoconvert_convert_convert (pad->convert, converted_frame, frame);
        pad->converted_buffer = converted_buf;
        gst_video_frame_unmap (frame);
      } else {
        converted_frame = frame;
      }

      pad->mixed_frame = converted_frame;
    }
  }

  mixer_klass->mix_frames (mix, &outframe);

  for (l = mix->sinkpads; l; l = l->next) {
    GstBasemixerPad *pad = l->data;

    if (pad->mixed_frame) {
      gst_video_frame_unmap (pad->mixed_frame);
      g_slice_free (GstVideoFrame, pad->mixed_frame);
      pad->mixed_frame = NULL;
    }

    if (pad->converted_buffer) {
      gst_buffer_unref (pad->converted_buffer);
      pad->converted_buffer = NULL;
    }
  }
  gst_video_frame_unmap (&outframe);

  return GST_FLOW_OK;
}

/* Perform qos calculations before processing the next frame. Returns TRUE if
 * the frame should be processed, FALSE if the frame can be dropped entirely */
static gint64
gst_basemixer_do_qos (GstBasemixer * mix, GstClockTime timestamp)
{
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR(mix);
  GstClockTime qostime, earliest_time;
  gdouble proportion;
  gint64 jitter;

  /* no timestamp, can't do QoS => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (timestamp))) {
    GST_LOG_OBJECT (mix, "invalid timestamp, can't do QoS, process frame");
    return -1;
  }

  /* get latest QoS observation values */
  gst_basemixer_read_qos (mix, &proportion, &earliest_time);

  /* skip qos if we have no observation (yet) => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (earliest_time))) {
    GST_LOG_OBJECT (mix, "no observation yet, process frame");
    return -1;
  }

  /* qos is done on running time */
  qostime =
      gst_segment_to_running_time (&agg->segment, GST_FORMAT_TIME, timestamp);

  /* see how our next timestamp relates to the latest qos timestamp */
  GST_LOG_OBJECT (mix, "qostime %" GST_TIME_FORMAT ", earliest %"
      GST_TIME_FORMAT, GST_TIME_ARGS (qostime), GST_TIME_ARGS (earliest_time));

  jitter = GST_CLOCK_DIFF (qostime, earliest_time);
  if (qostime != GST_CLOCK_TIME_NONE && jitter > 0) {
    GST_DEBUG_OBJECT (mix, "we are late, drop frame");
    return jitter;
  }

  GST_LOG_OBJECT (mix, "process frame");
  return jitter;
}

static GstFlowReturn
gst_basemixer_aggregate (GstBaseAggregator * agg)
{
  GstFlowReturn ret;
  GstBasemixer *mix = GST_BASE_MIXER (agg);
  GstClockTime output_start_time, output_end_time;
  GstBuffer *outbuf = NULL;
  gint res;
  gint64 jitter;

  /* If we're not negotiated yet... */
  if (GST_VIDEO_INFO_FORMAT (&mix->info) == GST_VIDEO_FORMAT_UNKNOWN)
    return GST_FLOW_NOT_NEGOTIATED;

  if (gst_pad_check_reconfigure (agg->srcpad))
    gst_basemixer_update_src_caps (mix);

  if (mix->send_caps) {
    gst_base_aggregator_set_src_caps(agg, mix->current_caps);
    mix->send_caps = FALSE;
  }

  GST_BASE_MIXER_LOCK (mix);

  if (agg->segment.position == -1)
    output_start_time = agg->segment.start;
  else
    output_start_time = agg->segment.position;

  output_end_time =
      mix->ts_offset + gst_util_uint64_scale_round (mix->nframes + 1,
      GST_SECOND * GST_VIDEO_INFO_FPS_D (&mix->info),
      GST_VIDEO_INFO_FPS_N (&mix->info)) + agg->segment.start;

  if (G_UNLIKELY (mix->pending_tags)) {
    gst_pad_push_event (agg->srcpad, gst_event_new_tag (mix->pending_tags));
    mix->pending_tags = NULL;
  }

  if (agg->segment.stop != -1)
    output_end_time = MIN (output_end_time, agg->segment.stop);

  res = gst_basemixer_fill_queues (mix, output_start_time, output_end_time);

  if (res == 0) {
    GST_DEBUG_OBJECT (mix, "Need more data for decisions");
    ret = GST_FLOW_OK;
    goto done;
  } else if (res == -1) {
    GST_BASE_MIXER_UNLOCK (mix);
    GST_DEBUG_OBJECT (mix, "All sinkpads are EOS -- forwarding");
    gst_pad_push_event (agg->srcpad, gst_event_new_eos ());
    ret = GST_FLOW_EOS;
    goto done_unlocked;
  } else if (res == -2) {
    GST_DEBUG_OBJECT (mix, "Error collecting buffers");
    ret = GST_FLOW_ERROR;
    goto done;
  }

  jitter = gst_basemixer_do_qos (mix, output_start_time);
  if (jitter <= 0) {
    ret =
        gst_basemixer_blend_buffers (mix, output_start_time,
        output_end_time, &outbuf);
    mix->qos_processed++;
  } else {
    GstMessage *msg;

    mix->qos_dropped++;

    /* TODO: live */
    msg =
        gst_message_new_qos (GST_OBJECT_CAST (mix), FALSE,
        gst_segment_to_running_time (&agg->segment, GST_FORMAT_TIME,
            output_start_time), gst_segment_to_stream_time (&agg->segment,
            GST_FORMAT_TIME, output_start_time), output_start_time,
        output_end_time - output_start_time);
    gst_message_set_qos_values (msg, jitter, mix->proportion, 1000000);
    gst_message_set_qos_stats (msg, GST_FORMAT_BUFFERS, mix->qos_processed,
        mix->qos_dropped);
    gst_element_post_message (GST_ELEMENT_CAST (mix), msg);

    ret = GST_FLOW_OK;
  }

  agg->segment.position = output_end_time;
  mix->nframes++;

  GST_BASE_MIXER_UNLOCK (mix);
  if (outbuf) {
    GST_DEBUG_OBJECT (mix,
        "Pushing buffer with ts %" GST_TIME_FORMAT " and duration %"
        GST_TIME_FORMAT, GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)));
    ret = gst_base_aggregator_finish_buffer (agg, outbuf);
  }
  goto done_unlocked;

done:
  GST_BASE_MIXER_UNLOCK (mix);

done_unlocked:
  return ret;
}

/* FIXME, the duration query should reflect how long you will produce
 * data, that is the amount of stream time until you will emit EOS.
 *
 * For synchronized mixing this is always the max of all the durations
 * of upstream since we emit EOS when all of them finished.
 *
 * We don't do synchronized mixing so this really depends on where the
 * streams where punched in and what their relative offsets are against
 * eachother which we can get from the first timestamps we see.
 *
 * When we add a new stream (or remove a stream) the duration might
 * also become invalid again and we need to post a new DURATION
 * message to notify this fact to the parent.
 * For now we take the max of all the upstream elements so the simple
 * cases work at least somewhat.
 */
static gboolean
gst_basemixer_query_duration (GstBasemixer * mix, GstQuery * query)
{
  GValue item = { 0 };
  gint64 max;
  gboolean res;
  GstFormat format;
  GstIterator *it;
  gboolean done;

  /* parse format */
  gst_query_parse_duration (query, &format, NULL);

  max = -1;
  res = TRUE;
  done = FALSE;

  /* Take maximum of all durations */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad;
        gint64 duration;

        pad = g_value_get_object (&item);

        /* ask sink peer for duration */
        res &= gst_pad_peer_query_duration (pad, format, &duration);
        /* take max from all valid return values */
        if (res) {
          /* valid unknown length, stop searching */
          if (duration == -1) {
            max = duration;
            done = TRUE;
          }
          /* else see if bigger than current max */
          else if (duration > max)
            max = duration;
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        max = -1;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (res) {
    /* and store the max */
    GST_DEBUG_OBJECT (mix, "Total duration in format %s: %"
        GST_TIME_FORMAT, gst_format_get_name (format), GST_TIME_ARGS (max));
    gst_query_set_duration (query, format, max);
  }

  return res;
}

static gboolean
gst_basemixer_query_latency (GstBasemixer * mix, GstQuery * query)
{
  GstClockTime min, max;
  gboolean live;
  gboolean res;
  GstIterator *it;
  gboolean done;
  GValue item = { 0 };

  res = TRUE;
  done = FALSE;
  live = FALSE;
  min = 0;
  max = GST_CLOCK_TIME_NONE;

  /* Take maximum of all latency values */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad = g_value_get_object (&item);
        GstQuery *peerquery;
        GstClockTime min_cur, max_cur;
        gboolean live_cur;

        peerquery = gst_query_new_latency ();

        /* Ask peer for latency */
        res &= gst_pad_peer_query (pad, peerquery);

        /* take max from all valid return values */
        if (res) {
          gst_query_parse_latency (peerquery, &live_cur, &min_cur, &max_cur);

          if (min_cur > min)
            min = min_cur;

          if (max_cur != GST_CLOCK_TIME_NONE &&
              ((max != GST_CLOCK_TIME_NONE && max_cur > max) ||
                  (max == GST_CLOCK_TIME_NONE)))
            max = max_cur;

          live = live || live_cur;
        }

        gst_query_unref (peerquery);
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        live = FALSE;
        min = 0;
        max = GST_CLOCK_TIME_NONE;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (res) {
    /* store the results */
    GST_DEBUG_OBJECT (mix, "Calculated total latency: live %s, min %"
        GST_TIME_FORMAT ", max %" GST_TIME_FORMAT,
        (live ? "yes" : "no"), GST_TIME_ARGS (min), GST_TIME_ARGS (max));
    gst_query_set_latency (query, live, min, max);
  }

  return res;
}

static gboolean
gst_basemixer_src_query (GstBaseAggregator * agg, GstQuery * query)
{
  GstBasemixer *mix = GST_BASE_MIXER (agg);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      switch (format) {
        case GST_FORMAT_TIME:
          gst_query_set_position (query, format,
              gst_segment_to_stream_time (&agg->segment, GST_FORMAT_TIME,
                  agg->segment.position));
          res = TRUE;
          break;
        default:
          break;
      }
      break;
    }
    case GST_QUERY_DURATION:
      res = gst_basemixer_query_duration (mix, query);
      break;
    case GST_QUERY_LATENCY:
      res = gst_basemixer_query_latency (mix, query);
      break;
    case GST_QUERY_CAPS:
      res = GST_BASE_AGGREGATOR_CLASS (parent_class)->src_query (agg, query);
      break;
    default:
      /* FIXME, needs a custom query handler because we have multiple
       * sinkpads */
      res = FALSE;
      break;
  }
  return res;
}

static gboolean
gst_basemixer_src_event (GstBaseAggregator * agg, GstEvent * event)
{
  GstBasemixer *mix = GST_BASE_MIXER (agg);
  gboolean result;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      GstQOSType type;
      GstClockTimeDiff diff;
      GstClockTime timestamp;
      gdouble proportion;

      gst_event_parse_qos (event, &type, &proportion, &diff, &timestamp);

      gst_basemixer_update_qos (mix, proportion, diff, timestamp);

      result = gst_basemixer_push_sink_event (mix, event);
      break;
    }
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat fmt;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      GSList *l;
      gdouble abs_rate;

      /* parse the seek parameters */
      gst_event_parse_seek (event, &rate, &fmt, &flags, &start_type,
          &start, &stop_type, &stop);

      if (rate <= 0.0) {
        GST_DEBUG_OBJECT (mix, "Negative rates not supported yet");
        result = FALSE;
        gst_event_unref (event);
        break;
      }

      GST_DEBUG_OBJECT (mix, "Handling SEEK event");

      abs_rate = ABS (rate);

      GST_BASE_MIXER_LOCK (mix);
      for (l = mix->sinkpads; l; l = l->next) {
        GstBasemixerPad *p = l->data;

        if (flags & GST_SEEK_FLAG_FLUSH) {
          gst_buffer_replace (&p->buffer, NULL);
          p->start_time = p->end_time = -1;
          continue;
        }

        /* Convert to the output segment rate */
        if (ABS (agg->segment.rate) != abs_rate) {
          if (ABS (agg->segment.rate) != 1.0 && p->buffer) {
            p->start_time /= ABS (agg->segment.rate);
            p->end_time /= ABS (agg->segment.rate);
          }
          if (abs_rate != 1.0 && p->buffer) {
            p->start_time *= abs_rate;
            p->end_time *= abs_rate;
          }
        }
      }
      GST_BASE_MIXER_UNLOCK (mix);

      agg->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;

      gst_basemixer_reset_qos (mix);

      result = GST_BASE_AGGREGATOR_CLASS (parent_class)->src_event (agg, event);
      break;
    }
    case GST_EVENT_NAVIGATION:
      /* navigation is rather pointless. */
      result = FALSE;
      gst_event_unref (event);
      break;
    default:
      /* just forward the rest for now */
      result = gst_basemixer_push_sink_event (mix, event);
      break;
  }

  return result;
}

static gboolean
gst_basemixer_src_setcaps (GstPad * pad, GstBasemixer * mix, GstCaps * caps)
{
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR(mix);
  gboolean ret = FALSE;
  GstVideoInfo info;

  GST_INFO_OBJECT (pad, "set src caps: %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&info, caps))
    goto done;

  ret = TRUE;

  GST_BASE_MIXER_LOCK (mix);

  if (GST_VIDEO_INFO_FPS_N (&mix->info) != GST_VIDEO_INFO_FPS_N (&info) ||
      GST_VIDEO_INFO_FPS_D (&mix->info) != GST_VIDEO_INFO_FPS_D (&info)) {
    if (agg->segment.position != -1) {
      mix->ts_offset = agg->segment.position - agg->segment.start;
      mix->nframes = 0;
    }
    gst_basemixer_reset_qos (mix);
  }

  mix->info = info;

  GST_BASE_MIXER_UNLOCK (mix);

  if (mix->current_caps == NULL ||
      gst_caps_is_equal (caps, mix->current_caps) == FALSE) {
    gst_caps_replace (&mix->current_caps, caps);
    mix->send_caps = TRUE;
  }

done:
  return ret;
}

static GstFlowReturn
gst_basemixer_sink_clip (GstBaseAggregator * agg,
    GstBaseAggregatorPad * bpad, GstBuffer * buf, GstBuffer ** outbuf)
{
  GstBasemixerPad *pad = GST_BASE_MIXER_PAD (bpad);
  GstClockTime start_time, end_time;

  start_time = GST_BUFFER_TIMESTAMP (buf);
  if (start_time == -1) {
    GST_DEBUG_OBJECT (pad, "Timestamped buffers required!");
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  end_time = GST_BUFFER_DURATION (buf);
  if (end_time == -1 && GST_VIDEO_INFO_FPS_N (&pad->info) != 0)
    end_time =
        gst_util_uint64_scale_int_round (GST_SECOND,
        GST_VIDEO_INFO_FPS_D (&pad->info), GST_VIDEO_INFO_FPS_N (&pad->info));
  if (end_time == -1) {
    *outbuf = buf;
    return GST_FLOW_OK;
  }

  start_time = MAX (start_time, bpad->segment.start);
  start_time =
      gst_segment_to_running_time (&bpad->segment, GST_FORMAT_TIME, start_time);

  end_time += GST_BUFFER_TIMESTAMP (buf);
  if (bpad->segment.stop != -1)
    end_time = MIN (end_time, bpad->segment.stop);
  end_time =
      gst_segment_to_running_time (&bpad->segment, GST_FORMAT_TIME, end_time);

  /* Convert to the output segment rate */
  if (ABS (agg->segment.rate) != 1.0) {
    start_time *= ABS (agg->segment.rate);
    end_time *= ABS (agg->segment.rate);
  }

  if (bpad->buffer != NULL && end_time < pad->end_time) {
    gst_buffer_unref (buf);
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = buf;
  return GST_FLOW_OK;
}

static gboolean
gst_basemixer_flush (GstBaseAggregator * agg)
{
  GstBasemixer *mix = GST_BASE_MIXER (agg);

  if (mix->pending_tags) {
    gst_tag_list_unref (mix->pending_tags);
    mix->pending_tags = NULL;
  }
  return TRUE;
}

static gboolean
gst_basemixer_sink_event (GstBaseAggregator * agg, GstBaseAggregatorPad * bpad,
    GstEvent * event)
{
  GstBasemixer *mix = GST_BASE_MIXER (agg);
  GstBasemixerPad *pad = GST_BASE_MIXER_PAD (bpad);
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (pad, "Got %s event on pad %s:%s",
      GST_EVENT_TYPE_NAME (event), GST_DEBUG_PAD_NAME (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret =
          gst_basemixer_pad_sink_setcaps (GST_PAD (pad), GST_OBJECT (mix),
          caps);
      gst_event_unref (event);
      event = NULL;
      break;
    }
    case GST_EVENT_SEGMENT:{
      GstSegment seg;
      gst_event_copy_segment (event, &seg);

      g_assert (seg.format == GST_FORMAT_TIME);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      mix->newseg_pending = TRUE;

      gst_basemixer_reset_qos (mix);
      GST_BASE_MIXER_LOCK (mix);
      gst_buffer_replace (&pad->buffer, NULL);
      GST_BASE_MIXER_UNLOCK (mix);
      pad->start_time = -1;
      pad->end_time = -1;

      agg->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;
      break;
    case GST_EVENT_TAG:
    {
      /* collect tags here so we can push them out when we collect data */
      GstTagList *tags;

      gst_event_parse_tag (event, &tags);
      tags = gst_tag_list_merge (mix->pending_tags, tags, GST_TAG_MERGE_APPEND);
      if (mix->pending_tags)
        gst_tag_list_unref (mix->pending_tags);
      mix->pending_tags = gst_tag_list_ref(tags);
      event = NULL;
      break;
    }
    default:
      break;
  }

  if (event != NULL)
    return GST_BASE_AGGREGATOR_CLASS (parent_class)->pad_event (agg, bpad,
        event);

  return ret;
}

static gboolean
forward_event_func (GValue * item, GValue * ret, GstEvent * event)
{
  GstPad *pad = g_value_get_object (item);
  gst_event_ref (event);
  GST_DEBUG_OBJECT (pad, "About to send event %s", GST_EVENT_TYPE_NAME (event));
  if (!gst_pad_push_event (pad, event)) {
    g_value_set_boolean (ret, FALSE);
    GST_WARNING_OBJECT (pad, "Sending event  %p (%s) failed.",
        event, GST_EVENT_TYPE_NAME (event));
  } else {
    GST_LOG_OBJECT (pad, "Sent event  %p (%s).",
        event, GST_EVENT_TYPE_NAME (event));
  }
  return TRUE;
}

static gboolean
gst_basemixer_push_sink_event (GstBasemixer * mix, GstEvent * event)
{
  GstIterator *it;
  GValue vret = { 0 };

  GST_LOG_OBJECT (mix, "Forwarding event %p (%s)", event,
      GST_EVENT_TYPE_NAME (event));

  g_value_init (&vret, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vret, TRUE);
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  gst_iterator_fold (it, (GstIteratorFoldFunction) forward_event_func, &vret,
      event);
  gst_iterator_free (it);
  gst_event_unref (event);

  return g_value_get_boolean (&vret);
}

/* GstElement vmethods */
static GstStateChangeReturn
gst_basemixer_change_state (GstElement * element, GstStateChange transition)
{
  GstBaseAggregator *agg = GST_BASE_AGGREGATOR(element);
  GstBasemixer *mix = GST_BASE_MIXER (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      mix->send_stream_start = TRUE;
      mix->send_caps = TRUE;
      gst_segment_init (&agg->segment, GST_FORMAT_TIME);
      gst_caps_replace (&mix->current_caps, NULL);
      GST_LOG_OBJECT (mix, "starting collectpads");
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_LOG_OBJECT (mix, "stopping collectpads");
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_basemixer_reset (mix);
      break;
    default:
      break;
  }

  return ret;
}

static GstPad *
gst_basemixer_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * req_name, const GstCaps * caps)
{
  GstBasemixer *mix;
  GstBasemixerPad *mixpad;

  mix = GST_BASE_MIXER (element);

  mixpad = (GstBasemixerPad *)
      GST_ELEMENT_CLASS (parent_class)->request_new_pad (element, templ,
      req_name, caps);

  if (mixpad == NULL)
    return NULL;
  mixpad->zorder = mix->numpads;

  mixpad->start_time = -1;
  mixpad->end_time = -1;

  /* Keep an internal list of mixpads for zordering */
  GST_BASE_MIXER_LOCK (mix);
  mix->sinkpads = g_slist_insert_sorted (mix->sinkpads, mixpad,
      (GCompareFunc) pad_zorder_compare);
  mix->numpads++;
  GST_BASE_MIXER_UNLOCK (mix);

  gst_child_proxy_child_added (GST_CHILD_PROXY (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));

  return GST_PAD (mixpad);
}

static void
gst_basemixer_release_pad (GstElement * element, GstPad * pad)
{
  GstBasemixer *mix = NULL;
  GstBasemixerPad *mixpad;
  gboolean update_caps;

  mix = GST_BASE_MIXER (element);

  GST_BASE_MIXER_LOCK (mix);
  if (G_UNLIKELY (g_slist_find (mix->sinkpads, pad) == NULL)) {
    g_warning ("Unknown pad %s", GST_PAD_NAME (pad));
    goto error;
  }

  mixpad = GST_BASE_MIXER_PAD (pad);

  if (mixpad->convert)
    videoconvert_convert_free (mixpad->convert);

  mix->sinkpads = g_slist_remove (mix->sinkpads, pad);
  gst_child_proxy_child_removed (GST_CHILD_PROXY (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));
  mix->numpads--;

  // FUCKING FIXME after talking with thib
  //GST_COLLECT_PADS_STREAM_LOCK (mix->collect);
  gst_basemixer_update_converters (mix);
  //GST_COLLECT_PADS_STREAM_UNLOCK (mix->collect);

  update_caps = GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN;
  GST_BASE_MIXER_UNLOCK (mix);

  GST_ELEMENT_CLASS (parent_class)->release_pad (GST_ELEMENT (mix), pad);

  if (update_caps)
    gst_basemixer_update_src_caps (mix);

  return;
error:
  GST_BASE_MIXER_UNLOCK (mix);
}

/* GObject vmethods */
static void
gst_basemixer_finalize (GObject * o)
{
  GstBasemixer *mix = GST_BASE_MIXER (o);

  g_mutex_clear (&mix->lock);
  g_mutex_clear (&mix->setcaps_lock);

  G_OBJECT_CLASS (parent_class)->finalize (o);
}

static void
gst_basemixer_dispose (GObject * o)
{
  GstBasemixer *mix = GST_BASE_MIXER (o);
  GSList *tmp;

  for (tmp = mix->sinkpads; tmp; tmp = tmp->next) {
    GstBasemixerPad *mixpad = tmp->data;

    if (mixpad->convert)
      videoconvert_convert_free (mixpad->convert);
  }

  if (mix->pending_tags) {
    gst_tag_list_unref (mix->pending_tags);
    mix->pending_tags = NULL;
  }

  gst_caps_replace (&mix->current_caps, NULL);
}

static void
gst_basemixer_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_basemixer_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstChildProxy implementation */
static GObject *
gst_basemixer_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstBasemixer *mix = GST_BASE_MIXER (child_proxy);
  GObject *obj;

  GST_BASE_MIXER_LOCK (mix);
  if ((obj = g_slist_nth_data (mix->sinkpads, index)))
    g_object_ref (obj);
  GST_BASE_MIXER_UNLOCK (mix);
  return obj;
}

static guint
gst_basemixer_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  guint count = 0;
  GstBasemixer *mix = GST_BASE_MIXER (child_proxy);

  GST_BASE_MIXER_LOCK (mix);
  count = mix->numpads;
  GST_BASE_MIXER_UNLOCK (mix);
  GST_INFO_OBJECT (mix, "Children Count: %d", count);
  return count;
}

static void
gst_basemixer_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  GST_INFO ("intializing child proxy interface");
  iface->get_child_by_index = gst_basemixer_child_proxy_get_child_by_index;
  iface->get_children_count = gst_basemixer_child_proxy_get_children_count;
}

/* GObject boilerplate */
static void
gst_basemixer_class_init (GstBasemixerClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseAggregatorClass *agg_class = (GstBaseAggregatorClass *) klass;

  GST_DEBUG_CATEGORY_INIT (gst_basemixer_debug, "basemixer", 0, "base mixer");

  gobject_class->finalize = gst_basemixer_finalize;
  gobject_class->dispose = gst_basemixer_dispose;

  gobject_class->get_property = gst_basemixer_get_property;
  gobject_class->set_property = gst_basemixer_set_property;

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_basemixer_request_new_pad);
  gstelement_class->release_pad = GST_DEBUG_FUNCPTR (gst_basemixer_release_pad);
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_basemixer_change_state);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gst_element_class_set_static_metadata (gstelement_class, "Video mixer 2",
      "Filter/Editor/Video",
      "Mix multiple video streams", "Wim Taymans <wim@fluendo.com>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  agg_class->sinkpads_type = GST_TYPE_BASE_MIXER_PAD;
  agg_class->pad_query = gst_basemixer_sink_query;
  agg_class->pad_event = gst_basemixer_sink_event;
  agg_class->flush = gst_basemixer_flush;
  agg_class->clip = gst_basemixer_sink_clip;
  agg_class->aggregate = gst_basemixer_aggregate;
  agg_class->src_event = gst_basemixer_src_event;
  agg_class->src_query = gst_basemixer_src_query;

  /* Register the pad class */
  g_type_class_ref (GST_TYPE_BASE_MIXER_PAD);
}

static void
gst_basemixer_init (GstBasemixer * mix)
{
  mix->current_caps = NULL;
  mix->pending_tags = NULL;

  g_mutex_init (&mix->lock);
  g_mutex_init (&mix->setcaps_lock);
  /* initialize variables */
  gst_basemixer_reset (mix);
}
