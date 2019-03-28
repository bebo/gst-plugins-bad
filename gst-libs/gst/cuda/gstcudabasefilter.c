/* GStreamer
 * Copyright (C) <2019> Seungha Yang <seungha.yang@navercorp.com>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstcudabasefilter.h"
#include <gst/cuda/gstcuda_private.h>

GST_DEBUG_CATEGORY_STATIC (gst_cuda_base_filter_debug);
#define GST_CAT_DEFAULT gst_cuda_base_filter_debug

enum
{
  PROP_0,
  PROP_DEVICE_ID,
};

#define DEFAULT_DEVICE_ID -1

#define gst_cuda_base_filter_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE (GstCudaBaseFilter, gst_cuda_base_filter,
    GST_TYPE_BASE_TRANSFORM);

static void gst_cuda_base_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_cuda_base_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_cuda_base_filter_dispose (GObject * object);
static void gst_cuda_base_filter_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_cuda_base_filter_start (GstBaseTransform * trans);
static gboolean gst_cuda_base_filter_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_cuda_base_filter_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf);
static gboolean gst_cuda_base_filter_get_unit_size (GstBaseTransform * trans,
    GstCaps * caps, gsize * size);
static gboolean
gst_cuda_base_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
static gboolean
gst_cuda_base_filter_decide_allocation (GstBaseTransform * trans,
    GstQuery * query);
static gboolean
gst_cuda_base_filter_query (GstBaseTransform * trans, GstPadDirection direction,
    GstQuery * query);
static GstFlowReturn
gst_cuda_base_filter_transform_frame_default (GstCudaBaseFilter * filter,
    GstCudaMemoryTarget in_target, GstVideoFrame * in_frame,
    GstCudaMemoryTarget out_target, GstVideoFrame * out_frame);


static void
gst_cuda_base_filter_class_init (GstCudaBaseFilterClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseTransformClass *trans_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_cuda_base_filter_set_property;
  gobject_class->get_property = gst_cuda_base_filter_get_property;
  gobject_class->dispose = gst_cuda_base_filter_dispose;

  g_object_class_install_property (gobject_class, PROP_DEVICE_ID,
      g_param_spec_int ("cuda-device-id",
          "Cuda Device ID",
          "Set the GPU device to use for operations (-1 = auto)",
          -1, G_MAXINT, DEFAULT_DEVICE_ID,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));

  element_class->set_context =
      GST_DEBUG_FUNCPTR (gst_cuda_base_filter_set_context);

  trans_class->passthrough_on_same_caps = TRUE;

  trans_class->start = GST_DEBUG_FUNCPTR (gst_cuda_base_filter_start);
  trans_class->set_caps = GST_DEBUG_FUNCPTR (gst_cuda_base_filter_set_caps);
  trans_class->transform = GST_DEBUG_FUNCPTR (gst_cuda_base_filter_transform);
  trans_class->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_cuda_base_filter_get_unit_size);
  trans_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_cuda_base_filter_propose_allocation);
  trans_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_cuda_base_filter_decide_allocation);
  trans_class->query = GST_DEBUG_FUNCPTR (gst_cuda_base_filter_query);

  klass->transform_frame =
      GST_DEBUG_FUNCPTR (gst_cuda_base_filter_transform_frame_default);

  GST_DEBUG_CATEGORY_INIT (gst_cuda_base_filter_debug,
      "cudabasefilter", 0, "cudabasefilter Element");
}

static void
gst_cuda_base_filter_init (GstCudaBaseFilter * filter)
{
  filter->device_id = DEFAULT_DEVICE_ID;

  filter->negotiated = FALSE;
}

static void
gst_cuda_base_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      filter->device_id = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cuda_base_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      g_value_set_int (value, filter->device_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cuda_base_filter_dispose (GObject * object)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (object);

  gst_clear_object (&filter->context);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_cuda_base_filter_set_context (GstElement * element, GstContext * context)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (element);

  gst_cuda_handle_set_context (element,
      context, &filter->context, filter->device_id);

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_cuda_base_filter_start (GstBaseTransform * trans)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);

  if (!gst_cuda_ensure_element_context (GST_ELEMENT_CAST (filter),
          &filter->context, filter->device_id)) {
    GST_ERROR_OBJECT (filter, "Failed to get CUDA context");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_cuda_base_filter_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);
  GstVideoInfo in_info, out_info;
  GstCudaBaseFilterClass *klass;
  gboolean res;

  if (!filter->context) {
    GST_ERROR_OBJECT (filter, "No available CUDA context");
    return FALSE;
  }

  /* input caps */
  if (!gst_video_info_from_caps (&in_info, incaps))
    goto invalid_caps;

  /* output caps */
  if (!gst_video_info_from_caps (&out_info, outcaps))
    goto invalid_caps;

  klass = GST_CUDA_BASE_FILTER_GET_CLASS (filter);
  if (klass->set_info)
    res = klass->set_info (filter, incaps, &in_info, outcaps, &out_info);
  else
    res = TRUE;

  if (res) {
    filter->in_info = in_info;
    filter->out_info = out_info;
  }

  filter->negotiated = res;

  return res;

  /* ERRORS */
invalid_caps:
  {
    GST_ERROR_OBJECT (filter, "invalid caps");
    filter->negotiated = FALSE;
    return FALSE;
  }
}

static gboolean
gst_cuda_base_filter_get_unit_size (GstBaseTransform * trans, GstCaps * caps,
    gsize * size)
{
  gboolean ret = FALSE;
  GstVideoInfo info;

  ret = gst_video_info_from_caps (&info, caps);
  if (ret)
    *size = GST_VIDEO_INFO_SIZE (&info);

  return TRUE;
}

static GstFlowReturn
gst_cuda_base_filter_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);
  GstCudaBaseFilterClass *fclass = GST_CUDA_BASE_FILTER_GET_CLASS (filter);
  GstVideoFrame in_frame, out_frame;
  GstFlowReturn ret = GST_FLOW_OK;
  GstMapFlags in_map_flags, out_map_flags;
  GstMemory *mem;
  GstCudaMemoryTarget in_target, out_target;

  if (G_UNLIKELY (!filter->negotiated))
    goto unknown_format;

  in_map_flags = GST_MAP_READ | GST_VIDEO_FRAME_MAP_FLAG_NO_REF;
  out_map_flags = GST_MAP_WRITE | GST_VIDEO_FRAME_MAP_FLAG_NO_REF;

  in_target = out_target = GST_CUDA_MEMORY_TARGET_HOST;

  if (gst_buffer_n_memory (inbuf) == 1 &&
      (mem = gst_buffer_peek_memory (inbuf, 0)) && gst_is_cuda_memory (mem)) {
    if (gst_cuda_memory_get_target ((GstCudaMemory *) mem, &in_target) &&
        in_target == GST_CUDA_MEMORY_TARGET_DEVICE) {
      in_map_flags |= GST_MAP_CUDA;
    }
  }

  if (gst_buffer_n_memory (outbuf) == 1 &&
      (mem = gst_buffer_peek_memory (outbuf, 0)) && gst_is_cuda_memory (mem)) {
    if (gst_cuda_memory_get_target ((GstCudaMemory *) mem, &out_target) &&
        out_target == GST_CUDA_MEMORY_TARGET_DEVICE) {
      out_map_flags |= GST_MAP_CUDA;
    }
  }

  if (!gst_video_frame_map (&in_frame, &filter->in_info, inbuf, in_map_flags))
    goto invalid_buffer;

  if (!gst_video_frame_map (&out_frame, &filter->out_info, outbuf,
          out_map_flags)) {
    gst_video_frame_unmap (&in_frame);
    goto invalid_buffer;
  }

  ret =
      fclass->transform_frame (filter, in_target, &in_frame, out_target,
      &out_frame);

  gst_video_frame_unmap (&out_frame);
  gst_video_frame_unmap (&in_frame);

  return ret;

  /* ERRORS */
unknown_format:
  {
    GST_ELEMENT_ERROR (filter, CORE, NOT_IMPLEMENTED, (NULL),
        ("unknown format"));
    return GST_FLOW_NOT_NEGOTIATED;
  }
invalid_buffer:
  {
    GST_ELEMENT_WARNING (trans, CORE, NOT_IMPLEMENTED, (NULL),
        ("invalid video buffer received"));
    return GST_FLOW_OK;
  }
}

static GstFlowReturn
gst_cuda_base_filter_transform_frame_default (GstCudaBaseFilter * filter,
    GstCudaMemoryTarget in_target, GstVideoFrame * in_frame,
    GstCudaMemoryTarget out_target, GstVideoFrame * out_frame)
{
  gint i;
  GstFlowReturn ret = GST_FLOW_OK;

  if (in_target == GST_CUDA_MEMORY_TARGET_DEVICE ||
      out_target == GST_CUDA_MEMORY_TARGET_DEVICE) {
    CUmemorytype in_type, out_type;

    if (!gst_cuda_context_push (filter->context)) {
      GST_ELEMENT_ERROR (filter, LIBRARY, FAILED, (NULL),
          ("Cannot push CUDA context"));

      return GST_FLOW_ERROR;
    }

    if (in_target == GST_CUDA_MEMORY_TARGET_DEVICE)
      in_type = CU_MEMORYTYPE_DEVICE;
    else
      in_type = CU_MEMORYTYPE_HOST;

    if (out_target == GST_CUDA_MEMORY_TARGET_DEVICE)
      out_type = CU_MEMORYTYPE_DEVICE;
    else
      out_type = CU_MEMORYTYPE_HOST;

    for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (in_frame); i++) {
      CUDA_MEMCPY2D param = { 0, };
      guint width, height;

      width = GST_VIDEO_FRAME_COMP_WIDTH (in_frame, i) *
          GST_VIDEO_FRAME_COMP_PSTRIDE (in_frame, i);
      height = GST_VIDEO_FRAME_COMP_HEIGHT (in_frame, i);

      param.srcMemoryType = in_type;
      param.srcPitch = GST_VIDEO_FRAME_PLANE_STRIDE (in_frame, i);;
      if (in_type == CU_MEMORYTYPE_DEVICE)
        param.srcDevice =
            (CUdeviceptr) GST_VIDEO_FRAME_PLANE_DATA (in_frame, i);
      else
        param.srcHost = GST_VIDEO_FRAME_PLANE_DATA (in_frame, i);

      param.dstMemoryType = out_type;
      param.dstPitch = GST_VIDEO_FRAME_PLANE_STRIDE (out_frame, i);
      if (out_type == CU_MEMORYTYPE_DEVICE)
        param.dstDevice =
            (CUdeviceptr) GST_VIDEO_FRAME_PLANE_DATA (out_frame, i);
      else
        param.dstHost = GST_VIDEO_FRAME_PLANE_DATA (out_frame, i);

      param.WidthInBytes = width;
      param.Height = height;

      if (!gst_cuda_result (CuMemcpy2D (&param))) {
        gst_cuda_context_pop ();
        GST_ELEMENT_ERROR (filter, LIBRARY, FAILED, (NULL),
            ("Cannot upload input video frame"));

        return GST_FLOW_ERROR;
      }
    }

    gst_cuda_context_pop ();
  } else {
    for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (in_frame); i++) {
      if (!gst_video_frame_copy_plane (out_frame, in_frame, i)) {
        GST_ERROR_OBJECT (filter, "Couldn't copy %dth plane", i);

        return GST_FLOW_ERROR;
      }
    }
  }

  return ret;
}

static gboolean
gst_cuda_base_filter_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);
  GstVideoInfo info;
  GstBufferPool *pool;
  GstCaps *caps;
  guint size;

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation (trans,
          decide_query, query))
    return FALSE;

  /* passthrough, we're done */
  if (decide_query == NULL)
    return TRUE;

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  size = GST_VIDEO_INFO_SIZE (&info);

  if (gst_query_get_n_allocation_pools (query) == 0) {
    GstCapsFeatures *features;
    GstCudaMemoryTarget target = GST_CUDA_MEMORY_TARGET_HOST;
    GstStructure *config;

    features = gst_caps_get_features (caps, 0);

    if (features && gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY)) {
      GST_DEBUG_OBJECT (filter, "upstream support CUDA memory");
      target = GST_CUDA_MEMORY_TARGET_DEVICE;

      pool = gst_cuda_buffer_pool_new (filter->context, target);
    } else {
      pool = gst_video_buffer_pool_new ();
    }

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, 0, 0);

    gst_query_add_allocation_pool (query, pool, size, 0, 0);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;

    gst_object_unref (pool);
  }

  return TRUE;

  /* ERRORS */
config_failed:
  {
    GST_ERROR_OBJECT (filter, "failed to set config");
    gst_object_unref (pool);
    return FALSE;
  }
}

static gboolean
gst_cuda_base_filter_decide_allocation (GstBaseTransform * trans,
    GstQuery * query)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);
  GstCaps *outcaps = NULL;
  GstBufferPool *pool = NULL;
  guint size, min, max;
  GstStructure *config;
  GstCudaMemoryTarget target = GST_CUDA_MEMORY_TARGET_HOST;
  gboolean update_pool = FALSE;
  GstCapsFeatures *features;

  gst_query_parse_allocation (query, &outcaps, NULL);

  if (!outcaps)
    return FALSE;

  features = gst_caps_get_features (outcaps, 0);
  if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY)) {
    target = GST_CUDA_MEMORY_TARGET_DEVICE;
  }

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    if (pool && !GST_IS_CUDA_BUFFER_POOL (pool) &&
        target == GST_CUDA_MEMORY_TARGET_DEVICE) {
      /* when cuda device memory is supported, but pool is not cudabufferpool */
      gst_object_unref (pool);
      pool = NULL;
    }

    update_pool = TRUE;
  } else {
    GstVideoInfo vinfo;
    gst_video_info_from_caps (&vinfo, outcaps);
    size = GST_VIDEO_INFO_SIZE (&vinfo);
    min = max = 0;
  }

  if (!pool) {
    GST_DEBUG_OBJECT (filter, "create our pool");

    if (target == GST_CUDA_MEMORY_TARGET_DEVICE)
      pool = gst_cuda_buffer_pool_new (filter->context, target);
    else
      pool = gst_video_buffer_pool_new ();
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
  gst_buffer_pool_set_config (pool, config);
  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  gst_object_unref (pool);

  return GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (trans,
      query);
}

static gboolean
gst_cuda_base_filter_query (GstBaseTransform * trans, GstPadDirection direction,
    GstQuery * query)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (trans);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
    {
      gboolean ret;
      ret = gst_cuda_handle_context_query (GST_ELEMENT (filter), query,
          filter->context);
      if (ret)
        return TRUE;
      break;
    }
    default:
      break;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->query (trans, direction,
      query);
}
