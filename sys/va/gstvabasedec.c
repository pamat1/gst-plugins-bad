/* GStreamer
 * Copyright (C) 2020 Igalia, S.L.
 *     Author: Víctor Jáquez <vjaquez@igalia.com>
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
 * License along with this library; if not, write to the0
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "gstvabasedec.h"

#include "gstvaallocator.h"
#include "gstvacaps.h"
#include "gstvapool.h"
#include "gstvautils.h"
#include "gstvavideoformat.h"

#define GST_CAT_DEFAULT (base->debug_category)

#define parent_class gst_va_base_dec_parent_class
gpointer gst_va_base_dec_parent_class = NULL;

static gboolean
gst_va_base_dec_open (GstVideoDecoder * decoder)
{
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);
  GstVaBaseDecClass *klass = GST_VA_BASE_DEC_GET_CLASS (decoder);

  if (!gst_va_ensure_element_data (decoder, klass->render_device_path,
          &base->display))
    return FALSE;

  if (!base->decoder)
    base->decoder = gst_va_decoder_new (base->display, klass->codec);

  return (base->decoder != NULL);
}

gboolean
gst_va_base_dec_close (GstVideoDecoder * decoder)
{
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);

  gst_clear_object (&base->decoder);
  gst_clear_object (&base->display);

  return TRUE;
}

static gboolean
gst_va_base_dec_stop (GstVideoDecoder * decoder)
{
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);

  if (!gst_va_decoder_close (base->decoder))
    return FALSE;

  if (base->output_state)
    gst_video_codec_state_unref (base->output_state);
  base->output_state = NULL;

  if (base->other_pool)
    gst_buffer_pool_set_active (base->other_pool, FALSE);
  gst_clear_object (&base->other_pool);

  return GST_VIDEO_DECODER_CLASS (parent_class)->stop (decoder);
}

static GstCaps *
gst_va_base_dec_getcaps (GstVideoDecoder * decoder, GstCaps * filter)
{
  GstCaps *caps = NULL, *tmp;
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);

  if (base->decoder)
    caps = gst_va_decoder_get_sinkpad_caps (base->decoder);

  if (caps) {
    if (filter) {
      tmp = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (caps);
      caps = tmp;
    }
    GST_LOG_OBJECT (base, "Returning caps %" GST_PTR_FORMAT, caps);
  } else {
    caps = gst_video_decoder_proxy_getcaps (decoder, NULL, filter);
  }

  return caps;
}

static gboolean
gst_va_base_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:{
      return gst_va_handle_context_query (GST_ELEMENT_CAST (decoder), query,
          base->display);
    }
    case GST_QUERY_CAPS:{
      GstCaps *caps = NULL, *tmp, *filter = NULL;
      gboolean fixed_caps;

      gst_query_parse_caps (query, &filter);

      fixed_caps = GST_PAD_IS_FIXED_CAPS (GST_VIDEO_DECODER_SRC_PAD (decoder));

      if (!fixed_caps && base->decoder)
        caps = gst_va_decoder_get_srcpad_caps (base->decoder);
      if (caps) {
        if (filter) {
          tmp =
              gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
          gst_caps_unref (caps);
          caps = tmp;
        }

        GST_LOG_OBJECT (base, "Returning caps %" GST_PTR_FORMAT, caps);
        gst_query_set_caps_result (query, caps);
        gst_caps_unref (caps);
        ret = TRUE;
        break;
      }
      /* else jump to default */
    }
    default:
      ret = GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_va_base_dec_sink_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);

  if (GST_QUERY_TYPE (query) == GST_QUERY_CONTEXT) {
    return gst_va_handle_context_query (GST_ELEMENT_CAST (decoder), query,
        base->display);
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->sink_query (decoder, query);
}

static GstAllocator *
_create_allocator (GstVaBaseDec * base, GstCaps * caps)
{
  GstAllocator *allocator = NULL;

  if (gst_caps_is_dmabuf (caps))
    allocator = gst_va_dmabuf_allocator_new (base->display);
  else {
    GArray *surface_formats =
        gst_va_decoder_get_surface_formats (base->decoder);
    allocator = gst_va_allocator_new (base->display, surface_formats);
  }

  return allocator;
}

static void
_create_other_pool (GstVaBaseDec * base, GstAllocator * allocator,
    GstAllocationParams * params, GstCaps * caps, guint size)
{
  GstBufferPool *pool;
  GstStructure *config;

  gst_clear_object (&base->other_pool);

  GST_DEBUG_OBJECT (base, "making new other pool for copy");

  pool = gst_video_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
  gst_buffer_pool_config_set_allocator (config, allocator, params);
  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (base, "Couldn't configure other pool for copy.");
    gst_clear_object (&pool);
  }

  base->other_pool = pool;
}

/* 1. get allocator in query
 *    1.1 if allocator is not ours and downstream doesn't handle
 *        videometa, keep it for other_pool
 * 2. get pool in query
 *    2.1 if pool is not va, keep it as other_pool if downstream
 *        doesn't handle videometa or (it doesn't handle alignment and
 *        the stream needs cropping)
 *    2.2 if there's no pool in query and downstream doesn't handle
 *        videometa, create other_pool as GstVideoPool with the non-va
 *        from query and query's params
 * 3. create our allocator and pool if they aren't in query
 * 4. add or update pool and allocator in query
 * 5. set our custom pool configuration
 */
static gboolean
gst_va_base_dec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstAllocator *allocator = NULL, *other_allocator = NULL;
  GstAllocationParams other_params, params;
  GstBufferPool *pool = NULL;
  GstCaps *caps = NULL;
  GstVideoInfo info;
  GstVaBaseDec *base = GST_VA_BASE_DEC (decoder);
  guint size = 0, min, max;
  gboolean update_pool = FALSE, update_allocator = FALSE, has_videoalignment;

  g_assert (base->min_buffers > 0);

  gst_query_parse_allocation (query, &caps, NULL);

  if (!(caps && gst_video_info_from_caps (&info, caps)))
    goto wrong_caps;

  base->has_videometa = gst_query_find_allocation_meta (query,
      GST_VIDEO_META_API_TYPE, NULL);

  if (gst_query_get_n_allocation_params (query) > 0) {
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &other_params);
    if (allocator && !(GST_IS_VA_DMABUF_ALLOCATOR (allocator)
            || GST_IS_VA_ALLOCATOR (allocator))) {
      /* save the allocator for the other pool */
      other_allocator = allocator;
      allocator = NULL;
    }
    update_allocator = TRUE;
  } else {
    gst_allocation_params_init (&other_params);
  }

  gst_allocation_params_init (&params);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    if (pool) {
      if (!GST_IS_VA_POOL (pool)) {
        has_videoalignment = gst_buffer_pool_has_option (pool,
            GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
        if (!base->has_videometa || (!has_videoalignment && base->need_valign)) {
          GST_DEBUG_OBJECT (base,
              "keeping other pool for copy %" GST_PTR_FORMAT, pool);
          gst_object_replace ((GstObject **) & base->other_pool,
              (GstObject *) pool);
          gst_object_unref (pool);      /* decrease previous increase */
        }
        gst_clear_object (&pool);
      }
    }

    min += base->min_buffers;
    size = MAX (size, GST_VIDEO_INFO_SIZE (&info));

    update_pool = TRUE;
  } else {
    size = GST_VIDEO_INFO_SIZE (&info);

    if (!base->has_videometa && !gst_caps_is_vamemory (caps)) {
      _create_other_pool (base, other_allocator, &other_params, caps, size);
    } else {
      gst_clear_object (&other_allocator);
    }

    min = base->min_buffers;
    max = 0;
  }

  if (!allocator) {
    if (!(allocator = _create_allocator (base, caps)))
      return FALSE;
  }

  if (!pool)
    pool = gst_va_pool_new ();

  {
    GstStructure *config = gst_buffer_pool_get_config (pool);

    gst_buffer_pool_config_set_params (config, caps, size, min, max);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    if (base->need_valign) {
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (config, &base->valign);
    }

    gst_buffer_pool_config_set_va_allocation_params (config,
        VA_SURFACE_ATTRIB_USAGE_HINT_DECODER);

    if (!gst_buffer_pool_set_config (pool, config))
      return FALSE;
  }

  if (update_allocator)
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  else
    gst_query_add_allocation_param (query, allocator, &params);

  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  if (!base->has_videometa) {
    if ((base->copy_frames = gst_va_pool_requires_video_meta (pool))) {
      GST_INFO_OBJECT (base, "Raw frame copy enabled.");
      _create_other_pool (base, other_allocator, &other_params, caps, size);
    }
  }
  if (!base->copy_frames)
    gst_clear_object (&base->other_pool);

  gst_object_unref (allocator);
  gst_object_unref (pool);

  /* There's no need to chain decoder's method since all what is
   * needed is done. */
  return TRUE;

wrong_caps:
  {
    GST_WARNING_OBJECT (base, "No valid caps");
    return FALSE;
  }
}

static void
gst_va_base_dec_set_context (GstElement * element, GstContext * context)
{
  GstVaDisplay *old_display, *new_display;
  GstVaBaseDec *base = GST_VA_BASE_DEC (element);
  GstVaBaseDecClass *klass = GST_VA_BASE_DEC_GET_CLASS (base);
  gboolean ret;

  old_display = base->display ? gst_object_ref (base->display) : NULL;
  ret = gst_va_handle_set_context (element, context, klass->render_device_path,
      &base->display);
  new_display = base->display ? gst_object_ref (base->display) : NULL;

  if (!ret
      || (old_display && new_display && old_display != new_display
          && base->decoder)) {
    GST_ELEMENT_WARNING (base, RESOURCE, BUSY,
        ("Can't replace VA display while operating"), (NULL));
  }

  gst_clear_object (&old_display);
  gst_clear_object (&new_display);

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

void
gst_va_base_dec_init (GstVaBaseDec * base, GstDebugCategory * cat)
{
  base->debug_category = cat;
}

void
gst_va_base_dec_class_init (GstVaBaseDecClass * klass, GstVaCodecs codec,
    const gchar * render_device_path, GstCaps * sink_caps, GstCaps * src_caps,
    GstCaps * doc_src_caps, GstCaps * doc_sink_caps)
{
  GstPadTemplate *sink_pad_templ, *src_pad_templ;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gst_va_base_dec_parent_class = g_type_class_peek_parent (klass);

  klass->codec = codec;
  klass->render_device_path = g_strdup (render_device_path);

  sink_pad_templ = gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
      sink_caps);
  gst_element_class_add_pad_template (element_class, sink_pad_templ);

  if (doc_sink_caps) {
    gst_pad_template_set_documentation_caps (sink_pad_templ, doc_sink_caps);
    gst_caps_unref (doc_sink_caps);
  }

  src_pad_templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
      src_caps);
  gst_element_class_add_pad_template (element_class, src_pad_templ);

  if (doc_src_caps) {
    gst_pad_template_set_documentation_caps (src_pad_templ, doc_src_caps);
    gst_caps_unref (doc_src_caps);
  }

  element_class->set_context = GST_DEBUG_FUNCPTR (gst_va_base_dec_set_context);

  decoder_class->open = GST_DEBUG_FUNCPTR (gst_va_base_dec_open);
  decoder_class->close = GST_DEBUG_FUNCPTR (gst_va_base_dec_close);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_va_base_dec_stop);
  decoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_va_base_dec_getcaps);
  decoder_class->src_query = GST_DEBUG_FUNCPTR (gst_va_base_dec_src_query);
  decoder_class->sink_query = GST_DEBUG_FUNCPTR (gst_va_base_dec_sink_query);
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_va_base_dec_decide_allocation);
}

static GstVideoFormat
_default_video_format_from_chroma (guint chroma_type)
{
  switch (chroma_type) {
    case VA_RT_FORMAT_YUV420:
    case VA_RT_FORMAT_YUV422:
    case VA_RT_FORMAT_YUV444:
      return GST_VIDEO_FORMAT_NV12;
    case VA_RT_FORMAT_YUV420_10:
    case VA_RT_FORMAT_YUV422_10:
    case VA_RT_FORMAT_YUV444_10:
      return GST_VIDEO_FORMAT_P010_10LE;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
  }
}

/* Check whether the downstream supports VideoMeta; if not, we need to
 * fallback to the system memory. */
static gboolean
_downstream_has_video_meta (GstVaBaseDec * base, GstCaps * caps)
{
  GstQuery *query;
  gboolean ret = FALSE;

  query = gst_query_new_allocation (caps, FALSE);
  if (gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD (base), query))
    ret = gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_unref (query);

  return ret;
}

void
gst_va_base_dec_get_preferred_format_and_caps_features (GstVaBaseDec * base,
    GstVideoFormat * format, GstCapsFeatures ** capsfeatures)
{
  GstCaps *peer_caps, *preferred_caps = NULL;
  GstCapsFeatures *features;
  GstStructure *structure;
  const GValue *v_format;
  guint num_structures, i;
  gboolean is_any;

  g_return_if_fail (base);

  /* verify if peer caps is any */
  {
    peer_caps =
        gst_pad_peer_query_caps (GST_VIDEO_DECODER_SRC_PAD (base), NULL);
    is_any = gst_caps_is_any (peer_caps);
    gst_clear_caps (&peer_caps);
  }

  peer_caps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (base));
  GST_DEBUG_OBJECT (base, "Allowed caps %" GST_PTR_FORMAT, peer_caps);

  /* prefer memory:VASurface over other caps features */
  num_structures = gst_caps_get_size (peer_caps);
  for (i = 0; i < num_structures; i++) {
    features = gst_caps_get_features (peer_caps, i);
    structure = gst_caps_get_structure (peer_caps, i);

    if (gst_caps_features_is_any (features))
      continue;

    if (gst_caps_features_contains (features, "memory:VAMemory")) {
      preferred_caps = gst_caps_new_full (gst_structure_copy (structure), NULL);
      gst_caps_set_features_simple (preferred_caps,
          gst_caps_features_copy (features));
      break;
    }
  }

  if (!preferred_caps)
    preferred_caps = peer_caps;
  else
    gst_clear_caps (&peer_caps);

  if (gst_caps_is_empty (preferred_caps)) {
    if (capsfeatures)
      *capsfeatures = NULL;     /* system memory */
    if (format)
      *format = _default_video_format_from_chroma (base->rt_format);
    goto bail;
  }

  if (capsfeatures) {
    features = gst_caps_get_features (preferred_caps, 0);
    if (features) {
      *capsfeatures = gst_caps_features_copy (features);

      if (is_any
          && !gst_caps_features_is_equal (features,
              GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)
          && !_downstream_has_video_meta (base, preferred_caps)) {
        GST_INFO_OBJECT (base, "Downstream reports ANY caps but without"
            " VideoMeta support; fallback to system memory.");
        gst_caps_features_free (*capsfeatures);
        *capsfeatures = NULL;
      }
    } else {
      *capsfeatures = NULL;
    }
  }

  if (!format)
    goto bail;

  structure = gst_caps_get_structure (preferred_caps, 0);
  v_format = gst_structure_get_value (structure, "format");
  if (!v_format)
    *format = _default_video_format_from_chroma (base->rt_format);
  else if (G_VALUE_HOLDS_STRING (v_format))
    *format = gst_video_format_from_string (g_value_get_string (v_format));
  else if (GST_VALUE_HOLDS_LIST (v_format)) {
    guint num_values = gst_value_list_get_size (v_format);
    for (i = 0; i < num_values; i++) {
      GstVideoFormat fmt;
      const GValue *v_fmt = gst_value_list_get_value (v_format, i);
      if (!v_fmt)
        continue;
      fmt = gst_video_format_from_string (g_value_get_string (v_fmt));
      if (gst_va_chroma_from_video_format (fmt) == base->rt_format) {
        *format = fmt;
        break;
      }
    }
    if (i == num_values)
      *format = _default_video_format_from_chroma (base->rt_format);
  }

bail:
  gst_clear_caps (&preferred_caps);
}

gboolean
gst_va_base_dec_copy_output_buffer (GstVaBaseDec * base,
    GstVideoCodecFrame * codec_frame)
{
  GstVideoFrame src_frame;
  GstVideoFrame dest_frame;
  GstVideoInfo dest_vinfo;
  GstVideoInfo *src_vinfo;
  GstBuffer *buffer;
  GstFlowReturn ret;

  g_return_val_if_fail (base && base->output_state, FALSE);

  if (!base->other_pool)
    return FALSE;

  if (!gst_buffer_pool_set_active (base->other_pool, TRUE))
    return FALSE;

  src_vinfo = &base->output_state->info;
  gst_video_info_set_format (&dest_vinfo, GST_VIDEO_INFO_FORMAT (src_vinfo),
      base->width, base->height);

  ret = gst_buffer_pool_acquire_buffer (base->other_pool, &buffer, NULL);
  if (ret != GST_FLOW_OK)
    goto fail;

  if (!gst_video_frame_map (&src_frame, src_vinfo, codec_frame->output_buffer,
          GST_MAP_READ))
    goto fail;

  if (!gst_video_frame_map (&dest_frame, &dest_vinfo, buffer, GST_MAP_WRITE)) {
    gst_video_frame_unmap (&src_frame);
    goto fail;
  }

  /* gst_video_frame_copy can crop this, but does not know, so let
   * make it think it's all right */
  GST_VIDEO_INFO_WIDTH (&src_frame.info) = base->width;
  GST_VIDEO_INFO_HEIGHT (&src_frame.info) = base->height;

  if (!gst_video_frame_copy (&dest_frame, &src_frame)) {
    gst_video_frame_unmap (&src_frame);
    gst_video_frame_unmap (&dest_frame);
    goto fail;
  }

  gst_video_frame_unmap (&src_frame);
  gst_video_frame_unmap (&dest_frame);
  gst_buffer_replace (&codec_frame->output_buffer, buffer);
  gst_buffer_unref (buffer);

  return TRUE;

fail:
  GST_ERROR_OBJECT (base, "Failed copy output buffer.");
  return FALSE;
}
