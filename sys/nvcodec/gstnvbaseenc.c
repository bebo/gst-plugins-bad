/* GStreamer NVENC plugin
 * Copyright (C) 2015 Centricular Ltd
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
#include "config.h"
#endif

#include "gstnvbaseenc.h"

#include <gst/pbutils/codec-utils.h>

#include <string.h>

#define GST_CAT_DEFAULT gst_nvenc_debug

#if HAVE_NVENC_GST_GL
#include <cuda_gl_interop.h>
#include <cudaGL.h>
#include <gst/gl/gl.h>
#endif

/* TODO:
 *  - reset last_flow on FLUSH_STOP (seeking)
 */

/* This currently supports both 5.x and 6.x versions of the NvEncodeAPI.h
 * header which are mostly API compatible. */

#define SUPPORTED_GL_APIS GST_GL_API_OPENGL3

/* magic pointer value we can put in the async queue to signal shut down */
#define SHUTDOWN_COOKIE ((gpointer)GINT_TO_POINTER (1))

#define parent_class gst_nv_base_enc_parent_class
G_DEFINE_ABSTRACT_TYPE (GstNvBaseEnc, gst_nv_base_enc, GST_TYPE_VIDEO_ENCODER);

#define GST_TYPE_NV_PRESET (gst_nv_preset_get_type())
static GType
gst_nv_preset_get_type (void)
{
  static GType nv_preset_type = 0;

  static const GEnumValue presets[] = {
    {GST_NV_PRESET_DEFAULT, "Default", "default"},
    {GST_NV_PRESET_HP, "High Performance", "hp"},
    {GST_NV_PRESET_HQ, "High Quality", "hq"},
/*    {GST_NV_PRESET_BD, "BD", "bd"}, */
    {GST_NV_PRESET_LOW_LATENCY_DEFAULT, "Low Latency", "low-latency"},
    {GST_NV_PRESET_LOW_LATENCY_HQ, "Low Latency, High Quality",
        "low-latency-hq"},
    {GST_NV_PRESET_LOW_LATENCY_HP, "Low Latency, High Performance",
        "low-latency-hp"},
    {GST_NV_PRESET_LOSSLESS_DEFAULT, "Lossless", "lossless"},
    {GST_NV_PRESET_LOSSLESS_HP, "Lossless, High Performance", "lossless-hp"},
    {0, NULL, NULL},
  };

  if (!nv_preset_type) {
    nv_preset_type = g_enum_register_static ("GstNvPreset", presets);
  }
  return nv_preset_type;
}

static GUID
_nv_preset_to_guid (GstNvPreset preset)
{
  GUID null = { 0, };

  switch (preset) {
#define CASE(gst,nv) case G_PASTE(GST_NV_PRESET_,gst): return G_PASTE(G_PASTE(NV_ENC_PRESET_,nv),_GUID)
      CASE (DEFAULT, DEFAULT);
      CASE (HP, HP);
      CASE (HQ, HQ);
/*    CASE (BD, BD);*/
      CASE (LOW_LATENCY_DEFAULT, LOW_LATENCY_DEFAULT);
      CASE (LOW_LATENCY_HQ, LOW_LATENCY_HQ);
      CASE (LOW_LATENCY_HP, LOW_LATENCY_HQ);
      CASE (LOSSLESS_DEFAULT, LOSSLESS_DEFAULT);
      CASE (LOSSLESS_HP, LOSSLESS_HP);
#undef CASE
    default:
      return null;
  }
}

#define GST_TYPE_NV_RC_MODE (gst_nv_rc_mode_get_type())
static GType
gst_nv_rc_mode_get_type (void)
{
  static GType nv_rc_mode_type = 0;

  static const GEnumValue modes[] = {
    {GST_NV_RC_MODE_DEFAULT, "Default (from NVENC preset)", "default"},
    {GST_NV_RC_MODE_CONSTQP, "Constant Quantization", "constqp"},
    {GST_NV_RC_MODE_CBR, "Constant Bit Rate", "cbr"},
    {GST_NV_RC_MODE_VBR, "Variable Bit Rate", "vbr"},
    {GST_NV_RC_MODE_VBR_MINQP,
          "Variable Bit Rate (with minimum quantization parameter)",
        "vbr-minqp"},
    {0, NULL, NULL},
  };

  if (!nv_rc_mode_type) {
    nv_rc_mode_type = g_enum_register_static ("GstNvRCMode", modes);
  }
  return nv_rc_mode_type;
}

static NV_ENC_PARAMS_RC_MODE
_rc_mode_to_nv (GstNvRCMode mode)
{
  switch (mode) {
    case GST_NV_RC_MODE_DEFAULT:
      return -1;
#define CASE(gst,nv) case G_PASTE(GST_NV_RC_MODE_,gst): return G_PASTE(NV_ENC_PARAMS_RC_,nv)
      CASE (CONSTQP, CONSTQP);
      CASE (CBR, CBR);
      CASE (VBR, VBR);
      CASE (VBR_MINQP, VBR_MINQP);
#undef CASE
    default:
      return -1;
  }
}

enum
{
  PROP_0,
  PROP_DEVICE_ID,
  PROP_PRESET,
  PROP_BITRATE,
  PROP_RC_MODE,
  PROP_QP_MIN,
  PROP_QP_MAX,
  PROP_QP_CONST,
  PROP_GOP_SIZE,
  PROP_RC_LOOKAHEAD,
  PROP_NO_SCENECUT,             /* consistent naming with x264 */
  PROP_B_ADAPT,
  PROP_BFRAMES,
};

#define DEFAULT_DEVICE_ID -1
#define DEFAULT_PRESET GST_NV_PRESET_DEFAULT
#define DEFAULT_BITRATE 0
#define DEFAULT_RC_MODE GST_NV_RC_MODE_DEFAULT
#define DEFAULT_QP_MIN -1
#define DEFAULT_QP_MAX -1
#define DEFAULT_QP_CONST -1
#define DEFAULT_GOP_SIZE 75
#define DEFAULT_RC_LOOKAHEAD 0
/* default values of nvEncodeAPI.h */
#define DEFAULT_NO_SCENECUT TRUE
#define DEFAULT_B_ADAPT FALSE
#define DEFAULT_BFRAMES 0

/* This lock is needed to prevent the situation where multiple encoders are
 * initialised at the same time which appears to cause excessive CPU usage over
 * some period of time. */
G_LOCK_DEFINE_STATIC (initialization_lock);

#if HAVE_NVENC_GST_GL
typedef struct
{
  GstGLMemory *gl_mem[GST_VIDEO_MAX_PLANES];
  CUgraphicsResource cuda_texture;
  CUdeviceptr cuda_plane_pointers[GST_VIDEO_MAX_PLANES];
  gpointer cuda_pointer;
  gsize cuda_stride;
  gsize cuda_num_bytes;
  NV_ENC_REGISTER_RESOURCE nv_resource;
  NV_ENC_MAP_INPUT_RESOURCE nv_mapped_resource;

  /* whether nv_mapped_resource was mapped via NvEncMapInputResource()
   * and therefore should unmap via NvEncUnmapInputResource or not */
  gboolean mapped;
} NvBaseEncGLResource;
#endif

typedef struct
{
  NV_ENC_REGISTER_RESOURCE nv_resource;
  NV_ENC_MAP_INPUT_RESOURCE nv_mapped_resource;
  GstBuffer *buffer;            /* holds ref */
} NvBaseEncCudaResource;

typedef struct
{
  gpointer in_buf;
  gpointer out_buf;
} NvBaseEncFrameState;

static gboolean gst_nv_base_enc_open (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_close (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_start (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_stop (GstVideoEncoder * enc);
static void gst_nv_base_enc_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_nv_base_enc_sink_query (GstVideoEncoder * enc,
    GstQuery * query);
static gboolean gst_nv_base_enc_set_format (GstVideoEncoder * enc,
    GstVideoCodecState * state);
static GstFlowReturn gst_nv_base_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame);
static void gst_nv_base_enc_free_buffers (GstNvBaseEnc * nvenc);
static GstFlowReturn gst_nv_base_enc_finish (GstVideoEncoder * enc);
static void gst_nv_base_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_nv_base_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_nv_base_enc_finalize (GObject * obj);
static GstCaps *gst_nv_base_enc_getcaps (GstVideoEncoder * enc,
    GstCaps * filter);
static gboolean gst_nv_base_enc_stop_bitstream_thread (GstNvBaseEnc * nvenc,
    gboolean force);
static GstCaps *gst_nv_base_enc_get_supported_input_caps (GstNvBaseEnc * nvenc);
static gboolean gst_nv_base_enc_propose_allocation (GstVideoEncoder * enc,
    GstQuery * query);

static void
gst_nv_base_enc_class_init (GstNvBaseEncClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *videoenc_class = GST_VIDEO_ENCODER_CLASS (klass);

  gobject_class->set_property = gst_nv_base_enc_set_property;
  gobject_class->get_property = gst_nv_base_enc_get_property;
  gobject_class->finalize = gst_nv_base_enc_finalize;

  element_class->set_context = GST_DEBUG_FUNCPTR (gst_nv_base_enc_set_context);

  videoenc_class->open = GST_DEBUG_FUNCPTR (gst_nv_base_enc_open);
  videoenc_class->close = GST_DEBUG_FUNCPTR (gst_nv_base_enc_close);

  videoenc_class->start = GST_DEBUG_FUNCPTR (gst_nv_base_enc_start);
  videoenc_class->stop = GST_DEBUG_FUNCPTR (gst_nv_base_enc_stop);

  videoenc_class->set_format = GST_DEBUG_FUNCPTR (gst_nv_base_enc_set_format);
  videoenc_class->getcaps = GST_DEBUG_FUNCPTR (gst_nv_base_enc_getcaps);
  videoenc_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_nv_base_enc_handle_frame);
  videoenc_class->finish = GST_DEBUG_FUNCPTR (gst_nv_base_enc_finish);
  videoenc_class->sink_query = GST_DEBUG_FUNCPTR (gst_nv_base_enc_sink_query);
  videoenc_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_nv_base_enc_propose_allocation);

  g_object_class_install_property (gobject_class, PROP_DEVICE_ID,
      g_param_spec_int ("cuda-device-id",
          "Cuda Device ID",
          "Set the GPU device to use for operations (-1 = auto)",
          -1, G_MAXINT, DEFAULT_DEVICE_ID,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PRESET,
      g_param_spec_enum ("preset", "Encoding Preset",
          "Encoding Preset",
          GST_TYPE_NV_PRESET, DEFAULT_PRESET,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_RC_MODE,
      g_param_spec_enum ("rc-mode", "RC Mode", "Rate Control Mode",
          GST_TYPE_NV_RC_MODE, DEFAULT_RC_MODE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_MIN,
      g_param_spec_int ("qp-min", "Minimum Quantizer",
          "Minimum quantizer (-1 = from NVENC preset)", -1, 51, DEFAULT_QP_MIN,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_MAX,
      g_param_spec_int ("qp-max", "Maximum Quantizer",
          "Maximum quantizer (-1 = from NVENC preset)", -1, 51, DEFAULT_QP_MAX,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_CONST,
      g_param_spec_int ("qp-const", "Constant Quantizer",
          "Constant quantizer (-1 = from NVENC preset)", -1, 51,
          DEFAULT_QP_CONST,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_GOP_SIZE,
      g_param_spec_int ("gop-size", "GOP size",
          "Number of frames between intra frames (-1 = infinite)",
          -1, G_MAXINT, DEFAULT_GOP_SIZE,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_uint ("bitrate", "Bitrate",
          "Bitrate in kbit/sec (0 = from NVENC preset)", 0, 2000 * 1024,
          DEFAULT_BITRATE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_RC_LOOKAHEAD,
      g_param_spec_uint ("rc-lookahead", "Rate Control Lookahead",
          "Number of frames to look ahead for rate-control", 0, 32,
          DEFAULT_RC_LOOKAHEAD, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NO_SCENECUT,
      g_param_spec_boolean ("no-scenecut", "No Scene Cut",
          "Disable adaptive I-frame insertion at scene cuts "
          "(only has an effect when rc_lookahead is non-zero)",
          DEFAULT_NO_SCENECUT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_B_ADAPT,
      g_param_spec_boolean ("b-adapt", "B-Adapt",
          "Adaptive B-frame decision "
          "(only has an effect when rc_lookahead is non-zero)",
          DEFAULT_B_ADAPT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /* NOTE: not sure upper bound of bframes, which is dependent on driver */
  g_object_class_install_property (gobject_class, PROP_BFRAMES,
      g_param_spec_uint ("bframes", "B-Frames",
          "Number of B-frames between I and P "
          "(upper bound is dependent on driver)",
          0, 16, DEFAULT_BFRAMES, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean
_get_supported_input_formats (GstNvBaseEnc * nvenc)
{
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (nvenc);
  guint64 format_mask = 0;
  uint32_t i, num = 0;
  NV_ENC_BUFFER_FORMAT formats[64];
  GValue val = G_VALUE_INIT;

  if (nvenc->input_formats)
    return TRUE;

  NvEncGetInputFormats (nvenc->encoder, nvenc_class->codec_id, formats,
      G_N_ELEMENTS (formats), &num);

  for (i = 0; i < num; ++i) {
    GST_INFO_OBJECT (nvenc, "input format: 0x%08x", formats[i]);
    /* Apparently we can just ignore the tiled formats and can feed
     * it the respective untiled planar format instead ?! */
    switch (formats[i]) {
      case NV_ENC_BUFFER_FORMAT_NV12_PL:
#if defined (NV_ENC_BUFFER_FORMAT_NV12_TILED16x16)
      case NV_ENC_BUFFER_FORMAT_NV12_TILED16x16:
#endif
#if defined (NV_ENC_BUFFER_FORMAT_NV12_TILED64x16)
      case NV_ENC_BUFFER_FORMAT_NV12_TILED64x16:
#endif
        format_mask |= (1 << GST_VIDEO_FORMAT_NV12);
        break;
      case NV_ENC_BUFFER_FORMAT_YV12_PL:
#if defined(NV_ENC_BUFFER_FORMAT_YV12_TILED16x16)
      case NV_ENC_BUFFER_FORMAT_YV12_TILED16x16:
#endif
#if defined (NV_ENC_BUFFER_FORMAT_YV12_TILED64x16)
      case NV_ENC_BUFFER_FORMAT_YV12_TILED64x16:
#endif
        format_mask |= (1 << GST_VIDEO_FORMAT_YV12);
        break;
      case NV_ENC_BUFFER_FORMAT_IYUV_PL:
#if defined (NV_ENC_BUFFER_FORMAT_IYUV_TILED16x16)
      case NV_ENC_BUFFER_FORMAT_IYUV_TILED16x16:
#endif
#if defined (NV_ENC_BUFFER_FORMAT_IYUV_TILED64x16)
      case NV_ENC_BUFFER_FORMAT_IYUV_TILED64x16:
#endif
        format_mask |= (1 << GST_VIDEO_FORMAT_I420);
        break;
      case NV_ENC_BUFFER_FORMAT_YUV444_PL:
#if defined (NV_ENC_BUFFER_FORMAT_YUV444_TILED16x16)
      case NV_ENC_BUFFER_FORMAT_YUV444_TILED16x16:
#endif
#if defined (NV_ENC_BUFFER_FORMAT_YUV444_TILED64x16)
      case NV_ENC_BUFFER_FORMAT_YUV444_TILED64x16:
#endif
      {
        NV_ENC_CAPS_PARAM caps_param = { 0, };
        int yuv444_supported = 0;

        caps_param.version = NV_ENC_CAPS_PARAM_VER;
        caps_param.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;

        if (NvEncGetEncodeCaps (nvenc->encoder, nvenc_class->codec_id,
                &caps_param, &yuv444_supported) != NV_ENC_SUCCESS)
          yuv444_supported = 0;

        if (yuv444_supported)
          format_mask |= (1 << GST_VIDEO_FORMAT_Y444);
        break;
      }
      default:
        GST_FIXME ("unmapped input format: 0x%08x", formats[i]);
        break;
    }
  }

  if (format_mask == 0)
    return FALSE;

  GST_OBJECT_LOCK (nvenc);
  nvenc->input_formats = g_new0 (GValue, 1);

  /* process a second time so we can add formats in the order we want */
  g_value_init (nvenc->input_formats, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_STRING);
  if ((format_mask & (1 << GST_VIDEO_FORMAT_NV12))) {
    g_value_set_static_string (&val, "NV12");
    gst_value_list_append_value (nvenc->input_formats, &val);
  }
  if ((format_mask & (1 << GST_VIDEO_FORMAT_YV12))) {
    g_value_set_static_string (&val, "YV12");
    gst_value_list_append_value (nvenc->input_formats, &val);
  }
  if ((format_mask & (1 << GST_VIDEO_FORMAT_I420))) {
    g_value_set_static_string (&val, "I420");
    gst_value_list_append_value (nvenc->input_formats, &val);
  }
  if ((format_mask & (1 << GST_VIDEO_FORMAT_Y444))) {
    g_value_set_static_string (&val, "Y444");
    gst_value_list_append_value (nvenc->input_formats, &val);
  }
  g_value_unset (&val);

  {
    NV_ENC_CAPS_PARAM caps_param = { 0, };
    caps_param.version = NV_ENC_CAPS_PARAM_VER;
    caps_param.capsToQuery = NV_ENC_CAPS_SUPPORT_FIELD_ENCODING;

    if (NvEncGetEncodeCaps (nvenc->encoder, nvenc_class->codec_id,
            &caps_param, &nvenc->interlace_modes) != NV_ENC_SUCCESS)
      nvenc->interlace_modes = 0;
  }

  GST_OBJECT_UNLOCK (nvenc);

  return TRUE;
}

static gboolean
gst_nv_base_enc_open (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  if (!gst_cuda_ensure_element_context (GST_ELEMENT_CAST (enc),
          &nvenc->cuda_ctx, nvenc->cuda_device_id)) {
    GST_ERROR_OBJECT (nvenc, "failed to create CUDA context");
    return FALSE;
  }

  {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { 0, };
    NVENCSTATUS nv_ret;

    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.apiVersion = NVENCAPI_VERSION;
    params.device = gst_cuda_context_get_context (nvenc->cuda_ctx);
    params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    nv_ret = NvEncOpenEncodeSessionEx (&params, &nvenc->encoder);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR ("Failed to create NVENC encoder session, ret=%d", nv_ret);
      gst_clear_object (&nvenc->cuda_ctx);
      return FALSE;
    }
    GST_INFO ("created NVENC encoder %p", nvenc->encoder);
  }

  /* query supported input formats */
  if (!_get_supported_input_formats (nvenc)) {
    GST_WARNING_OBJECT (nvenc, "No supported input formats");
    gst_nv_base_enc_close (enc);
    return FALSE;
  }

  return TRUE;
}

static void
gst_nv_base_enc_set_context (GstElement * element, GstContext * context)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (element);

  if (gst_cuda_handle_set_context (element, context,
          &nvenc->cuda_ctx, nvenc->cuda_device_id)) {
    goto done;
  }
#if HAVE_NVENC_GST_GL
  gst_gl_handle_set_context (element, context,
      (GstGLDisplay **) & nvenc->display,
      (GstGLContext **) & nvenc->other_context);
  if (nvenc->display)
    gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
        SUPPORTED_GL_APIS);
#endif

done:
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_nv_base_enc_accept_caps (GstNvBaseEnc * nvenc, GstCaps * caps)
{
  gboolean ret = TRUE;
  gint i, size;
  GstCaps *supported, *acceptable;
  GstStructure *s;
  const gchar *mode;

  supported = gst_nv_base_enc_get_supported_input_caps (nvenc);
  acceptable = gst_caps_copy (supported);
  size = gst_caps_get_size (acceptable);

  for (i = 0; i < size; i++) {
    s = gst_caps_get_structure (acceptable, i);
    gst_structure_remove_field (s, "interlace-mode");
  }

  if (!gst_caps_is_subset (caps, acceptable)) {
    ret = FALSE;
    goto beach;
  }

  s = gst_caps_get_structure (caps, 0);

  if ((mode = gst_structure_get_string (s, "interlace-mode")) != NULL) {
    GstVideoInterlaceMode imode = gst_video_interlace_mode_from_string (mode);
    if (nvenc->interlace_modes == 0 &&
        imode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) {
      ret = FALSE;
    } else if (nvenc->interlace_modes >= 1 &&
        imode > GST_VIDEO_INTERLACE_MODE_MIXED) {
      ret = FALSE;
    }

    if (!ret) {
      GST_DEBUG_OBJECT (nvenc, "cannot support interlace-mode %s",
          gst_video_interlace_mode_to_string (imode));
    }
  }

beach:
  gst_caps_unref (supported);
  gst_caps_unref (acceptable);

  return ret;
}

static gboolean
gst_nv_base_enc_sink_query (GstVideoEncoder * enc, GstQuery * query)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:{
      gboolean ret;

      if (gst_cuda_handle_context_query (GST_ELEMENT (nvenc),
              query, nvenc->cuda_ctx))
        return TRUE;

#if HAVE_NVENC_GST_GL
      ret = gst_gl_handle_context_query ((GstElement *) nvenc, query,
          nvenc->display, NULL, nvenc->other_context);
      if (nvenc->display)
        gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
            SUPPORTED_GL_APIS);

      if (ret)
        return ret;
#endif
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:{
      GstCaps *caps;
      gboolean ret;

      gst_query_parse_accept_caps (query, &caps);
      ret = gst_nv_base_enc_accept_caps (nvenc, caps);

      gst_query_set_accept_caps_result (query, ret);

      return TRUE;
    }
    default:
      break;
  }

  return GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (enc, query);
}

static gboolean
gst_nv_base_enc_propose_allocation (GstVideoEncoder * enc, GstQuery * query)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstCaps *caps;
  GstVideoInfo info;
  GstBufferPool *pool;
  GstStructure *config;
  GstCudaMemoryTarget target = GST_CUDA_MEMORY_TARGET_HOST;
  GstCapsFeatures *features;

  GST_DEBUG_OBJECT (nvenc, "propose allocation");

  gst_query_parse_allocation (query, &caps, NULL);

  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (nvenc, "failed to get video info");
    return FALSE;
  }

  features = gst_caps_get_features (caps, 0);
  if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
    GST_FIXME_OBJECT (nvenc, "add support gl memory pool");
    goto done;
  } else if (features && gst_caps_features_contains (features,
          GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY)) {
    GST_DEBUG_OBJECT (nvenc, "upstream support CUDA memory");
    target = GST_CUDA_MEMORY_TARGET_DEVICE;
  } else {
    GST_DEBUG_OBJECT (nvenc, "use system memory");
    target = GST_CUDA_MEMORY_TARGET_HOST;
  }

  pool = gst_cuda_buffer_pool_new (nvenc->cuda_ctx, target);

  if (G_UNLIKELY (pool == NULL)) {
    GST_WARNING_OBJECT (nvenc, "cannot create CUDA buffer pool");
    goto done;
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, GST_VIDEO_INFO_SIZE (&info),
      nvenc->items->len, nvenc->items->len);

  gst_query_add_allocation_pool (query, pool, GST_VIDEO_INFO_SIZE (&info),
      nvenc->items->len, nvenc->items->len);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  if (!gst_buffer_pool_set_config (pool, config))
    goto error_pool_config;

  gst_object_unref (pool);

done:
  return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (enc,
      query);

error_pool_config:
  {
    if (pool)
      gst_object_unref (pool);
    GST_WARNING_OBJECT (nvenc, "failed to set config");
    return FALSE;
  }
}

static gboolean
gst_nv_base_enc_start (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  nvenc->internal_pool = g_async_queue_new ();
  nvenc->processing_queue = g_async_queue_new ();
  nvenc->items = g_array_new (FALSE, TRUE, sizeof (NvBaseEncFrameState));

  nvenc->last_flow = GST_FLOW_OK;

#if HAVE_NVENC_GST_GL
  {
    gst_gl_ensure_element_data (GST_ELEMENT (nvenc),
        (GstGLDisplay **) & nvenc->display,
        (GstGLContext **) & nvenc->other_context);
    if (nvenc->display)
      gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
          SUPPORTED_GL_APIS);
  }
#endif

  return TRUE;
}

static gboolean
gst_nv_base_enc_stop (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  gst_nv_base_enc_stop_bitstream_thread (nvenc, TRUE);

  gst_nv_base_enc_free_buffers (nvenc);

  if (nvenc->input_state) {
    gst_video_codec_state_unref (nvenc->input_state);
    nvenc->input_state = NULL;
  }

  if (nvenc->internal_pool) {
    g_async_queue_unref (nvenc->internal_pool);
    nvenc->internal_pool = NULL;
  }
  if (nvenc->processing_queue) {
    g_async_queue_unref (nvenc->processing_queue);
    nvenc->processing_queue = NULL;
  }
  if (nvenc->display) {
    gst_object_unref (nvenc->display);
    nvenc->display = NULL;
  }
  if (nvenc->other_context) {
    gst_object_unref (nvenc->other_context);
    nvenc->other_context = NULL;
  }

  return TRUE;
}

static GstCaps *
gst_nv_base_enc_get_supported_input_caps (GstNvBaseEnc * nvenc)
{
  GstCaps *caps;
  GstCaps *template_caps;

  template_caps =
      gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SINK_PAD (nvenc));
  caps = gst_caps_copy (template_caps);
  gst_caps_unref (template_caps);

  if (nvenc->input_formats != NULL) {
    GValue list = G_VALUE_INIT;
    GValue sval = G_VALUE_INIT;

    gst_caps_set_value (caps, "format", nvenc->input_formats);

    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&sval, G_TYPE_STRING);

    g_value_set_static_string (&sval, "progressive");
    gst_value_list_append_value (&list, &sval);
    if (nvenc->interlace_modes >= 1) {
      g_value_set_static_string (&sval, "interleaved");
      gst_value_list_append_value (&list, &sval);
      g_value_set_static_string (&sval, "mixed");
      gst_value_list_append_value (&list, &sval);
    }
    gst_caps_set_value (caps, "interlace-mode", &list);

    g_value_unset (&sval);
    g_value_unset (&list);
  }

  GST_LOG_OBJECT (nvenc, "  supported caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static GstCaps *
gst_nv_base_enc_getcaps (GstVideoEncoder * enc, GstCaps * filter)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstCaps *supported_incaps = NULL;
  GstCaps *caps;

  supported_incaps = gst_nv_base_enc_get_supported_input_caps (nvenc);

  caps = gst_video_encoder_proxy_getcaps (enc, supported_incaps, filter);
  gst_clear_caps (&supported_incaps);

  GST_DEBUG_OBJECT (nvenc, "  returning caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_nv_base_enc_close (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  if (nvenc->encoder) {
    if (NvEncDestroyEncoder (nvenc->encoder) != NV_ENC_SUCCESS)
      return FALSE;
    nvenc->encoder = NULL;
  }

  gst_clear_object (&nvenc->cuda_ctx);

  GST_OBJECT_LOCK (nvenc);
  if (nvenc->input_formats)
    g_value_unset (nvenc->input_formats);
  g_free (nvenc->input_formats);
  nvenc->input_formats = NULL;
  GST_OBJECT_UNLOCK (nvenc);

  if (nvenc->input_state) {
    gst_video_codec_state_unref (nvenc->input_state);
    nvenc->input_state = NULL;
  }

  if (nvenc->internal_pool != NULL) {
    g_assert (g_async_queue_length (nvenc->internal_pool) == 0);
    g_async_queue_unref (nvenc->internal_pool);
    nvenc->internal_pool = NULL;
  }

  return TRUE;
}

static void
gst_nv_base_enc_init (GstNvBaseEnc * nvenc)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (nvenc);

  nvenc->cuda_device_id = DEFAULT_DEVICE_ID;
  nvenc->preset_enum = DEFAULT_PRESET;
  nvenc->selected_preset = _nv_preset_to_guid (nvenc->preset_enum);
  nvenc->rate_control_mode = DEFAULT_RC_MODE;
  nvenc->qp_min = DEFAULT_QP_MIN;
  nvenc->qp_max = DEFAULT_QP_MAX;
  nvenc->qp_const = DEFAULT_QP_CONST;
  nvenc->bitrate = DEFAULT_BITRATE;
  nvenc->gop_size = DEFAULT_GOP_SIZE;
  nvenc->rc_lookahead = DEFAULT_RC_LOOKAHEAD;
  nvenc->no_scenecut = DEFAULT_NO_SCENECUT;
  nvenc->b_adapt = DEFAULT_B_ADAPT;
  nvenc->bframes = DEFAULT_BFRAMES;

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
}

static void
gst_nv_base_enc_finalize (GObject * obj)
{
  G_OBJECT_CLASS (gst_nv_base_enc_parent_class)->finalize (obj);
}

static GstVideoCodecFrame *
_find_frame_with_output_buffer (GstNvBaseEnc * nvenc, NV_ENC_OUTPUT_PTR out_buf)
{
  GList *l, *walk = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (nvenc));
  GstVideoCodecFrame *ret = NULL;

  for (l = walk; l; l = l->next) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) l->data;
    NvBaseEncFrameState *state = gst_video_codec_frame_get_user_data (frame);

    if (!state)
      continue;

    if (state->out_buf == out_buf) {
      ret = frame;
      break;
    }
  }

  if (ret)
    gst_video_codec_frame_ref (ret);

  g_list_free_full (walk, (GDestroyNotify) gst_video_codec_frame_unref);

  return ret;
}

static gpointer
gst_nv_base_enc_bitstream_thread (gpointer user_data)
{
  GstVideoEncoder *enc = user_data;
  GstNvBaseEnc *nvenc = user_data;

  /* overview of operation:
   * 1. retreive the next buffer submitted to the bitstream pool
   * 2. wait for that buffer to be ready from nvenc (LockBitsream)
   * 3. retreive the GstVideoCodecFrame associated with that buffer
   * 4. for each buffer in the frame
   * 4.1 (step 2): wait for that buffer to be ready from nvenc (LockBitsream)
   * 4.2 create an output GstBuffer from the nvenc buffers
   * 4.3 unlock the nvenc bitstream buffers UnlockBitsream
   * 5. finish_frame()
   * 6. cleanup
   */
  do {
    GstBuffer *buffer;
    NvBaseEncFrameState *state = NULL;
    GstVideoCodecFrame *frame = NULL;
    NVENCSTATUS nv_ret;
    GstFlowReturn flow = GST_FLOW_OK;

    {
      NV_ENC_LOCK_BITSTREAM lock_bs = { 0, };
      NV_ENC_OUTPUT_PTR out_buf;

      {
        /* get and lock bitstream buffers */
        GstVideoCodecFrame *tmp_frame;

        GST_LOG_OBJECT (enc, "wait for bitstream buffer..");

        /* assumes buffers are submitted in order */
        state = g_async_queue_pop (nvenc->processing_queue);
        if ((gpointer) state == SHUTDOWN_COOKIE)
          break;

        out_buf = state->out_buf;

        tmp_frame = _find_frame_with_output_buffer (nvenc, out_buf);
        g_assert (tmp_frame != NULL);
        if (frame)
          g_assert (frame == tmp_frame);
        frame = tmp_frame;

        state = gst_video_codec_frame_get_user_data (frame);

        g_assert (state->out_buf == out_buf);

        GST_LOG_OBJECT (nvenc, "waiting for output buffer %p to be ready",
            out_buf);

        lock_bs.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock_bs.outputBitstream = out_buf;
        lock_bs.doNotWait = 0;

        /* FIXME: this would need to be updated for other slice modes */
        lock_bs.sliceOffsets = NULL;

        gst_cuda_context_push (nvenc->cuda_ctx);
        nv_ret = NvEncLockBitstream (nvenc->encoder, &lock_bs);
        if (nv_ret != NV_ENC_SUCCESS) {
          /* FIXME: what to do here? */
          gst_cuda_context_pop ();
          GST_ELEMENT_ERROR (nvenc, STREAM, ENCODE, (NULL),
              ("Failed to lock bitstream buffer %p, ret %d",
                  lock_bs.outputBitstream, nv_ret));
          break;
        }

        GST_LOG_OBJECT (nvenc, "picture type %d", lock_bs.pictureType);

        /* copy into output buffer */
        buffer =
            gst_buffer_new_allocate (NULL, lock_bs.bitstreamSizeInBytes, NULL);
        gst_buffer_fill (buffer, 0, lock_bs.bitstreamBufferPtr,
            lock_bs.bitstreamSizeInBytes);

        if (lock_bs.pictureType == NV_ENC_PIC_TYPE_IDR) {
          GST_DEBUG_OBJECT (nvenc, "This is a keyframe");
          GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
        }

        /* TODO: use lock_bs.outputTimeStamp and lock_bs.outputDuration */
        /* TODO: check pts/dts is handled properly if there are B-frames */

        nv_ret = NvEncUnlockBitstream (nvenc->encoder, state->out_buf);
        if (nv_ret != NV_ENC_SUCCESS) {
          /* FIXME: what to do here? */
          gst_cuda_context_pop ();
          GST_ELEMENT_ERROR (nvenc, STREAM, ENCODE, (NULL),
              ("Failed to unlock bitstream buffer %p, ret %d",
                  lock_bs.outputBitstream, nv_ret));
          break;
        }
      }
    }

    frame->output_buffer = buffer;

    {
      gpointer in_buf = state->in_buf;
      g_assert (in_buf != NULL);

      switch (nvenc->input_type) {
#if HAVE_NVENC_GST_GL
        case GST_NVENC_INPUT_GL:{
          NvBaseEncGLResource *in_gl_resource = in_buf;

          nv_ret =
              NvEncUnmapInputResource (nvenc->encoder,
              in_gl_resource->nv_mapped_resource.mappedResource);

          in_gl_resource->mapped = FALSE;

          if (nv_ret != NV_ENC_SUCCESS) {
            GST_ERROR_OBJECT (nvenc,
                "Failed to unmap input resource %p, ret %d", in_gl_resource,
                nv_ret);
            break;
          }

          memset (&in_gl_resource->nv_mapped_resource, 0,
              sizeof (in_gl_resource->nv_mapped_resource));

          break;
        }
#endif
        case GST_NVENC_INPUT_CUDA:{
          NvBaseEncCudaResource *in_cuda_resource = in_buf;

          /* FIXME: handle erros */
          if (!gst_cuda_result (NvEncUnmapInputResource (nvenc->encoder,
                      in_cuda_resource->nv_mapped_resource.mappedResource))) {
            GST_ERROR_OBJECT (nvenc, "Failed to unmap input resource %p",
                in_cuda_resource);
          }

          if (!gst_cuda_result (NvEncUnregisterResource (nvenc->encoder,
                      in_cuda_resource->nv_resource.registeredResource))) {
            GST_ERROR_OBJECT (nvenc, "Failed to unregister input resource %p",
                in_cuda_resource);
          }

          gst_clear_buffer (&in_cuda_resource->buffer);
          break;
        }
        default:
          break;
      }
    }
    gst_cuda_context_pop ();

    g_async_queue_push (nvenc->internal_pool, state);

    flow = gst_video_encoder_finish_frame (enc, frame);
    frame = NULL;

    if (flow != GST_FLOW_OK) {
      GST_INFO_OBJECT (enc, "got flow %s", gst_flow_get_name (flow));
      g_atomic_int_set (&nvenc->last_flow, flow);
      break;
    }
  }
  while (TRUE);

  GST_INFO_OBJECT (nvenc, "exiting thread");

  return NULL;
}

static gboolean
gst_nv_base_enc_start_bitstream_thread (GstNvBaseEnc * nvenc)
{
  gchar *name = g_strdup_printf ("%s-read-bits", GST_OBJECT_NAME (nvenc));

  g_assert (nvenc->bitstream_thread == NULL);

  g_assert (g_async_queue_length (nvenc->processing_queue) == 0);

  nvenc->bitstream_thread =
      g_thread_try_new (name, gst_nv_base_enc_bitstream_thread, nvenc, NULL);

  g_free (name);

  if (nvenc->bitstream_thread == NULL)
    return FALSE;

  GST_INFO_OBJECT (nvenc, "started thread to read bitstream");
  return TRUE;
}

static gboolean
gst_nv_base_enc_stop_bitstream_thread (GstNvBaseEnc * nvenc, gboolean force)
{
  NvBaseEncFrameState *state;

  if (nvenc->bitstream_thread == NULL)
    return TRUE;

  if (force) {
    g_async_queue_lock (nvenc->internal_pool);
    g_async_queue_lock (nvenc->processing_queue);
    while ((state = g_async_queue_try_pop_unlocked (nvenc->processing_queue))) {
      GST_INFO_OBJECT (nvenc, "stole bitstream buffer %p from queue", state);
      g_async_queue_push_unlocked (nvenc->internal_pool, state);
    }
    g_async_queue_push_unlocked (nvenc->processing_queue, SHUTDOWN_COOKIE);
    g_async_queue_unlock (nvenc->internal_pool);
    g_async_queue_unlock (nvenc->processing_queue);
  } else {
    /* wait for encoder to drain the remaining buffers */
    g_async_queue_push_unlocked (nvenc->processing_queue, SHUTDOWN_COOKIE);
  }

  if (!force) {
    /* temporary unlock during finish, so other thread can find and push frame */
    GST_VIDEO_ENCODER_STREAM_UNLOCK (nvenc);
  }

  g_thread_join (nvenc->bitstream_thread);

  if (!force)
    GST_VIDEO_ENCODER_STREAM_LOCK (nvenc);

  nvenc->bitstream_thread = NULL;
  return TRUE;
}

static void
gst_nv_base_enc_reset_queues (GstNvBaseEnc * nvenc, gboolean refill)
{
  gpointer ptr;
  gint i;

  GST_INFO_OBJECT (nvenc, "clearing queues");

  while ((ptr = g_async_queue_try_pop (nvenc->internal_pool))) {
    /* do nothing */
  }
  while ((ptr = g_async_queue_try_pop (nvenc->processing_queue))) {
    /* do nothing */
  }

  if (refill) {
    GST_INFO_OBJECT (nvenc, "refilling buffer pools");
    for (i = 0; i < nvenc->items->len; ++i) {
      g_async_queue_push (nvenc->internal_pool, &g_array_index (nvenc->items,
              NvBaseEncFrameState, i));
    }
  }
}

static void
gst_nv_base_enc_free_buffers (GstNvBaseEnc * nvenc)
{
  NVENCSTATUS nv_ret;
  guint i;

  if (nvenc->encoder == NULL)
    return;

  gst_nv_base_enc_reset_queues (nvenc, FALSE);
  gst_cuda_context_push (nvenc->cuda_ctx);
  for (i = 0; i < nvenc->items->len; ++i) {
    NV_ENC_OUTPUT_PTR out_buf =
        g_array_index (nvenc->items, NvBaseEncFrameState, i).out_buf;

    switch (nvenc->input_type) {
#if HAVE_NVENC_GST_GL
      case GST_NVENC_INPUT_GL:{
        NvBaseEncGLResource *in_gl_resource =
            g_array_index (nvenc->items, NvBaseEncFrameState, i).in_buf;

        if (in_gl_resource->mapped) {
          GST_LOG_OBJECT (nvenc, "Unmap resource %p", in_gl_resource);

          nv_ret =
              NvEncUnmapInputResource (nvenc->encoder,
              in_gl_resource->nv_mapped_resource.mappedResource);

          if (nv_ret != NV_ENC_SUCCESS) {
            GST_ERROR_OBJECT (nvenc,
                "Failed to unmap input resource %p, ret %d", in_gl_resource,
                nv_ret);
          }
        }

        nv_ret =
            NvEncUnregisterResource (nvenc->encoder,
            in_gl_resource->nv_resource.registeredResource);
        if (nv_ret != NV_ENC_SUCCESS)
          GST_ERROR_OBJECT (nvenc, "Failed to unregister resource %p, ret %d",
              in_gl_resource, nv_ret);

        g_free (in_gl_resource);
      }
        break;
#endif
      case GST_NVENC_INPUT_CUDA:{
        NvBaseEncCudaResource *in_cuda_resource =
            g_array_index (nvenc->items, NvBaseEncFrameState, i).in_buf;

        gst_clear_buffer (&in_cuda_resource->buffer);
        g_free (in_cuda_resource);
      }
        break;
      case GST_NVENC_INPUT_HOST:{
        NV_ENC_INPUT_PTR in_buf =
            (NV_ENC_INPUT_PTR) g_array_index (nvenc->items, NvBaseEncFrameState,
            i).in_buf;

        GST_DEBUG_OBJECT (nvenc, "Destroying input buffer %p", in_buf);
        nv_ret = NvEncDestroyInputBuffer (nvenc->encoder, in_buf);
        if (nv_ret != NV_ENC_SUCCESS) {
          GST_ERROR_OBJECT (nvenc, "Failed to destroy input buffer %p, ret %d",
              in_buf, nv_ret);
        }
      }
        break;
      default:
        break;
    }

    GST_DEBUG_OBJECT (nvenc, "Destroying output bitstream buffer %p", out_buf);
    nv_ret = NvEncDestroyBitstreamBuffer (nvenc->encoder, out_buf);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR_OBJECT (nvenc, "Failed to destroy output buffer %p, ret %d",
          out_buf, nv_ret);
    }
  }

  gst_cuda_context_pop ();
  g_array_free (nvenc->items, TRUE);
  nvenc->items = NULL;
}

static inline guint
_get_plane_width (GstVideoInfo * info, guint plane)
{
  if (GST_VIDEO_INFO_IS_YUV (info))
    /* For now component width and plane width are the same and the
     * plane-component mapping matches
     */
    return GST_VIDEO_INFO_COMP_WIDTH (info, plane);
  else                          /* RGB, GRAY */
    return GST_VIDEO_INFO_WIDTH (info);
}

static inline guint
_get_plane_height (GstVideoInfo * info, guint plane)
{
  if (GST_VIDEO_INFO_IS_YUV (info))
    /* For now component width and plane width are the same and the
     * plane-component mapping matches
     */
    return GST_VIDEO_INFO_COMP_HEIGHT (info, plane);
  else                          /* RGB, GRAY */
    return GST_VIDEO_INFO_HEIGHT (info);
}

static inline gsize
_get_frame_data_height (GstVideoInfo * info)
{
  gsize ret = 0;
  gint i;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    ret += _get_plane_height (info, i);
  }

  return ret;
}

void
gst_nv_base_enc_set_max_encode_size (GstNvBaseEnc * nvenc, guint max_width,
    guint max_height)
{
  nvenc->max_encode_width = max_width;
  nvenc->max_encode_height = max_height;
}

void
gst_nv_base_enc_get_max_encode_size (GstNvBaseEnc * nvenc, guint * max_width,
    guint * max_height)
{
  *max_width = nvenc->max_encode_width;
  *max_height = nvenc->max_encode_height;
}

static void
gst_nv_base_enc_setup_rate_control (GstNvBaseEnc * nvenc,
    NV_ENC_INITIALIZE_PARAMS * params)
{
  params->encodeConfig->rcParams.rateControlMode =
      _rc_mode_to_nv (nvenc->rate_control_mode);

  if (nvenc->bitrate > 0) {
    /* FIXME: this produces larger bitrates?! */
    params->encodeConfig->rcParams.averageBitRate = nvenc->bitrate * 1024;
    params->encodeConfig->rcParams.maxBitRate = nvenc->bitrate * 1024;
  }

  if (nvenc->qp_const > 0) {
    params->encodeConfig->rcParams.constQP.qpInterB = nvenc->qp_const;
    params->encodeConfig->rcParams.constQP.qpInterP = nvenc->qp_const;
    params->encodeConfig->rcParams.constQP.qpIntra = nvenc->qp_const;
  }

  if (nvenc->qp_min >= 0) {
    params->encodeConfig->rcParams.enableMinQP = 1;
    params->encodeConfig->rcParams.minQP.qpInterB = nvenc->qp_min;
    params->encodeConfig->rcParams.minQP.qpInterP = nvenc->qp_min;
    params->encodeConfig->rcParams.minQP.qpIntra = nvenc->qp_min;
  }

  if (nvenc->qp_max >= 0) {
    params->encodeConfig->rcParams.enableMaxQP = 1;
    params->encodeConfig->rcParams.maxQP.qpInterB = nvenc->qp_max;
    params->encodeConfig->rcParams.maxQP.qpInterP = nvenc->qp_max;
    params->encodeConfig->rcParams.maxQP.qpIntra = nvenc->qp_max;
  }

  if (nvenc->rc_lookahead > 0) {
    params->encodeConfig->rcParams.enableLookahead = 1;
    params->encodeConfig->rcParams.lookaheadDepth = nvenc->rc_lookahead;
    params->encodeConfig->rcParams.disableIadapt = nvenc->no_scenecut;
    params->encodeConfig->rcParams.disableBadapt = !nvenc->b_adapt;
    GST_DEBUG_OBJECT (nvenc,
        "set rc_lookahead: %u, no-scenecut: %s, B-adapt: %s",
        nvenc->rc_lookahead, nvenc->no_scenecut ? "true" : "false",
        nvenc->b_adapt ? "true" : "false");
  }
}

/* following calculation references a NVIDIA's contribution to ffmpeg
 * too large input pool size might waste GPU resources */
static guint
gst_nv_base_enc_calculate_num_input_buffer (GstNvBaseEnc * nvenc,
    NV_ENC_INITIALIZE_PARAMS * params)
{
  guint num;

  /* at least 4 buffers are required (ref. nvEncodeAPI.h)
   * multiply by 2 for number of NVENCs on gpu (hardcode to 2)
   * another multiply by 2 to avoid blocking next PBB group
   */
  num = MAX (4, params->encodeConfig->frameIntervalP * 2 * 2);

  if (nvenc->rc_lookahead > 0) {
    /* +1 is to account for lkd_bound calculation later
     * +4 is to allow sufficient pipelining with lookahead
     */
    num = MAX (num, params->encodeConfig->frameIntervalP + 1 + 4);
  }

  /* hardcoded upper bound */
  num = MIN (64, num);

  return num;
}

static gboolean
gst_nv_base_enc_set_format (GstVideoEncoder * enc, GstVideoCodecState * state)
{
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (enc);
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstVideoInfo *info = &state->info;
  GstVideoCodecState *old_state = nvenc->input_state;
  NV_ENC_RECONFIGURE_PARAMS reconfigure_params = { 0, };
  NV_ENC_INITIALIZE_PARAMS init_params = { 0, };
  NV_ENC_INITIALIZE_PARAMS *params;
  NV_ENC_PRESET_CONFIG preset_config = { 0, };
  NVENCSTATUS nv_ret;

  g_atomic_int_set (&nvenc->reconfig, FALSE);

  if (old_state) {
    reconfigure_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params = &reconfigure_params.reInitEncodeParams;
  } else {
    params = &init_params;
  }

  params->version = NV_ENC_INITIALIZE_PARAMS_VER;
  params->encodeGUID = nvenc_class->codec_id;
  params->encodeWidth = GST_VIDEO_INFO_WIDTH (info);
  params->encodeHeight = GST_VIDEO_INFO_HEIGHT (info);

  {
    guint32 n_presets;
    GUID *presets;
    guint32 i;

    nv_ret =
        NvEncGetEncodePresetCount (nvenc->encoder,
        params->encodeGUID, &n_presets);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Failed to get encoder presets"));
      return FALSE;
    }

    presets = g_new0 (GUID, n_presets);
    nv_ret =
        NvEncGetEncodePresetGUIDs (nvenc->encoder,
        params->encodeGUID, presets, n_presets, &n_presets);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Failed to get encoder presets"));
      g_free (presets);
      return FALSE;
    }

    for (i = 0; i < n_presets; i++) {
      if (gst_nvenc_cmp_guid (presets[i], nvenc->selected_preset))
        break;
    }
    g_free (presets);
    if (i >= n_presets) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Selected preset not supported"));
      return FALSE;
    }

    params->presetGUID = nvenc->selected_preset;
  }

  params->enablePTD = 1;
  if (!old_state) {
    /* this sets the required buffer size and the maximum allowed size on
     * subsequent reconfigures */
    /* FIXME: propertise this */
    params->maxEncodeWidth = GST_VIDEO_INFO_WIDTH (info);
    params->maxEncodeHeight = GST_VIDEO_INFO_HEIGHT (info);
    gst_nv_base_enc_set_max_encode_size (nvenc, params->maxEncodeWidth,
        params->maxEncodeHeight);
  } else {
    guint max_width, max_height;

    gst_nv_base_enc_get_max_encode_size (nvenc, &max_width, &max_height);

    if (GST_VIDEO_INFO_WIDTH (info) > max_width
        || GST_VIDEO_INFO_HEIGHT (info) > max_height) {
      GST_ELEMENT_ERROR (nvenc, STREAM, FORMAT, ("%s", "Requested stream "
              "size is larger than the maximum configured size"), (NULL));
      return FALSE;
    }
  }

  preset_config.version = NV_ENC_PRESET_CONFIG_VER;
  preset_config.presetCfg.version = NV_ENC_CONFIG_VER;

  nv_ret =
      NvEncGetEncodePresetConfig (nvenc->encoder,
      params->encodeGUID, params->presetGUID, &preset_config);
  if (nv_ret != NV_ENC_SUCCESS) {
    GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
        ("Failed to get encode preset configuration: %d", nv_ret));
    return FALSE;
  }

  params->encodeConfig = &preset_config.presetCfg;

  if (GST_VIDEO_INFO_IS_INTERLACED (info)) {
    if (nvenc->interlace_modes == 0) {
      GST_ERROR_OBJECT (nvenc,
          "driver couldn't support interlace mode encoding");
      return FALSE;
    }

    if (GST_VIDEO_INFO_INTERLACE_MODE (info) ==
        GST_VIDEO_INTERLACE_MODE_INTERLEAVED
        || GST_VIDEO_INFO_INTERLACE_MODE (info) ==
        GST_VIDEO_INTERLACE_MODE_MIXED) {
      preset_config.presetCfg.frameFieldMode =
          NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
    }
  }

  if (info->fps_d > 0 && info->fps_n > 0) {
    params->frameRateNum = info->fps_n;
    params->frameRateDen = info->fps_d;
  } else {
    GST_FIXME_OBJECT (nvenc, "variable framerate");
  }

  gst_nv_base_enc_setup_rate_control (nvenc, params);

  if (nvenc->gop_size < 0) {
    params->encodeConfig->gopLength = NVENC_INFINITE_GOPLENGTH;
    params->encodeConfig->frameIntervalP = 1;
  } else if (nvenc->gop_size == 0) {
    /* zero means intra-only */
    params->encodeConfig->gopLength = 1;
    params->encodeConfig->frameIntervalP = 0;
  } else {
    /* frameIntervalP = 0: I, 1: IPP, 2: IBP, 3: IBBP ... */
    if (nvenc->bframes > 0) {
      params->encodeConfig->frameIntervalP = nvenc->bframes + 1;
    }

    params->encodeConfig->gopLength = nvenc->gop_size;
  }

  g_assert (nvenc_class->set_encoder_config);
  if (!nvenc_class->set_encoder_config (nvenc, state, params->encodeConfig)) {
    GST_ERROR_OBJECT (enc, "Subclass failed to set encoder configuration");
    return FALSE;
  }

  G_LOCK (initialization_lock);
  if (old_state) {
    nv_ret = NvEncReconfigureEncoder (nvenc->encoder, &reconfigure_params);
  } else {
    nv_ret = NvEncInitializeEncoder (nvenc->encoder, params);
  }
  G_UNLOCK (initialization_lock);

  if (nv_ret != NV_ENC_SUCCESS) {
    GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
        ("Failed to %sinit encoder: %d", old_state ? "re" : "", nv_ret));
    return FALSE;
  }
  GST_INFO_OBJECT (nvenc, "configured encoder");

  if (!old_state) {
    nvenc->input_info = *info;
  }

  if (nvenc->input_state)
    gst_video_codec_state_unref (nvenc->input_state);
  nvenc->input_state = gst_video_codec_state_ref (state);
  GST_INFO_OBJECT (nvenc, "configured encoder");

  /* now allocate some buffers only on first configuration */
  if (!old_state) {
    GstCapsFeatures *features;
    guint i;
    guint input_width, input_height;
    guint n_bufs;

    input_width = GST_VIDEO_INFO_WIDTH (info);
    input_height = GST_VIDEO_INFO_HEIGHT (info);

    /* input buffers */
    n_bufs = gst_nv_base_enc_calculate_num_input_buffer (nvenc, params);
    g_array_set_size (nvenc->items, n_bufs);

    features = gst_caps_get_features (state->caps, 0);
#if HAVE_NVENC_GST_GL
    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
      nvenc->input_type = GST_NVENC_INPUT_GL;

      gst_cuda_context_push (nvenc->cuda_ctx);
      for (i = 0; i < n_bufs; ++i) {
        NvBaseEncGLResource *in_gl_resource = g_new0 (NvBaseEncGLResource, 1);
        CUresult cu_ret;

        memset (&in_gl_resource->nv_resource, 0,
            sizeof (in_gl_resource->nv_resource));
        memset (&in_gl_resource->nv_mapped_resource, 0,
            sizeof (in_gl_resource->nv_mapped_resource));

        /* scratch buffer for non-contigious planer into a contigious buffer */
        cu_ret =
            CuMemAllocPitch ((CUdeviceptr *) & in_gl_resource->cuda_pointer,
            &in_gl_resource->cuda_stride, input_width,
            _get_frame_data_height (info), 16);
        if (cu_ret != CUDA_SUCCESS) {
          const gchar *err;

          CuGetErrorString (cu_ret, &err);
          GST_ERROR_OBJECT (nvenc, "failed to alocate cuda scratch buffer "
              "ret %d error :%s", cu_ret, err);
          g_assert_not_reached ();
        }

        in_gl_resource->nv_resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        in_gl_resource->nv_resource.resourceType =
            NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        in_gl_resource->nv_resource.width = input_width;
        in_gl_resource->nv_resource.height = input_height;
        in_gl_resource->nv_resource.pitch = in_gl_resource->cuda_stride;
        in_gl_resource->nv_resource.bufferFormat =
            gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info));
        in_gl_resource->nv_resource.resourceToRegister =
            in_gl_resource->cuda_pointer;

        nv_ret =
            NvEncRegisterResource (nvenc->encoder,
            &in_gl_resource->nv_resource);
        if (nv_ret != NV_ENC_SUCCESS)
          GST_ERROR_OBJECT (nvenc, "Failed to register resource %p, ret %d",
              in_gl_resource, nv_ret);

        g_array_index (nvenc->items, NvBaseEncFrameState, i).in_buf =
            in_gl_resource;
      }

      gst_cuda_context_pop ();
    } else
#endif
    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY)) {
      GST_DEBUG_OBJECT (nvenc, "Upstream provides CUDA memory");
      nvenc->input_type = GST_NVENC_INPUT_CUDA;

      for (i = 0; i < n_bufs; ++i) {
        NvBaseEncCudaResource *in_cuda_resource =
            g_new0 (NvBaseEncCudaResource, 1);

        g_array_index (nvenc->items, NvBaseEncFrameState, i).in_buf =
            in_cuda_resource;
      }
    } else {
      nvenc->input_type = GST_NVENC_INPUT_HOST;

      for (i = 0; i < n_bufs; ++i) {
        NV_ENC_CREATE_INPUT_BUFFER cin_buf = { 0, };

        cin_buf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;

        cin_buf.width = input_width;
        cin_buf.height = input_height;

        cin_buf.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
        cin_buf.bufferFmt =
            gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info));

        nv_ret = NvEncCreateInputBuffer (nvenc->encoder, &cin_buf);

        if (nv_ret != NV_ENC_SUCCESS) {
          GST_WARNING_OBJECT (enc, "Failed to allocate input buffer: %d",
              nv_ret);
          /* FIXME: clean up */
          return FALSE;
        }


        GST_INFO_OBJECT (nvenc, "allocated  input buffer %2d: %p", i,
            cin_buf.inputBuffer);

        g_array_index (nvenc->items, NvBaseEncFrameState, i).in_buf =
            cin_buf.inputBuffer;
      }
    }

    /* output buffers */
    for (i = 0; i < n_bufs; ++i) {
      NV_ENC_CREATE_BITSTREAM_BUFFER cout_buf = { 0, };

      cout_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

      /* 1 MB should be large enough to hold most output frames.
       * NVENC will automatically increase this if it's not enough. */
      cout_buf.size = 1024 * 1024;
      cout_buf.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

      G_LOCK (initialization_lock);
      nv_ret = NvEncCreateBitstreamBuffer (nvenc->encoder, &cout_buf);
      G_UNLOCK (initialization_lock);

      if (nv_ret != NV_ENC_SUCCESS) {
        GST_WARNING_OBJECT (enc, "Failed to allocate input buffer: %d", nv_ret);
        /* FIXME: clean up */
        return FALSE;
      }

      GST_INFO_OBJECT (nvenc, "allocated output buffer %2d: %p", i,
          cout_buf.bitstreamBuffer);
      g_array_index (nvenc->items, NvBaseEncFrameState, i).out_buf =
          cout_buf.bitstreamBuffer;

      g_async_queue_push (nvenc->internal_pool, &g_array_index (nvenc->items,
              NvBaseEncFrameState, i));
    }

#if 0
    /* Get SPS/PPS */
    {
      NV_ENC_SEQUENCE_PARAM_PAYLOAD seq_param = { 0 };
      uint32_t seq_size = 0;

      seq_param.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
      seq_param.spsppsBuffer = g_alloca (1024);
      seq_param.inBufferSize = 1024;
      seq_param.outSPSPPSPayloadSize = &seq_size;

      nv_ret = NvEncGetSequenceParams (nvenc->encoder, &seq_param);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_WARNING_OBJECT (enc, "Failed to retrieve SPS/PPS: %d", nv_ret);
        return FALSE;
      }

      /* FIXME: use SPS/PPS */
      GST_MEMDUMP_OBJECT (enc, "SPS/PPS", seq_param.spsppsBuffer, seq_size);
    }
#endif
  }

  g_assert (nvenc_class->set_src_caps);
  if (!nvenc_class->set_src_caps (nvenc, state)) {
    GST_ERROR_OBJECT (nvenc, "Subclass failed to set output caps");
    /* FIXME: clean up */
    return FALSE;
  }

  return TRUE;
}

static inline guint
_plane_get_n_components (GstVideoInfo * info, guint plane)
{
  switch (GST_VIDEO_INFO_FORMAT (info)) {
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_AYUV:
      return 4;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_RGB16:
    case GST_VIDEO_FORMAT_BGR16:
      return 3;
    case GST_VIDEO_FORMAT_GRAY16_BE:
    case GST_VIDEO_FORMAT_GRAY16_LE:
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
      return 2;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
      return plane == 0 ? 1 : 2;
    case GST_VIDEO_FORMAT_GRAY8:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_Y41B:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
      return 1;
    default:
      g_assert_not_reached ();
      return 1;
  }
}

#if HAVE_NVENC_GST_GL
typedef struct
{
  GstNvBaseEnc *nvenc;
  GstVideoCodecFrame *frame;
  GstVideoInfo *info;
  NvBaseEncGLResource *in_gl_resource;
} NvBaseEncGLMap;

static void
_map_gl_input_buffer (GstGLContext * context, NvBaseEncGLMap * data)
{
  guint8 *data_pointer;
  guint i;
  CUDA_MEMCPY2D param;

  gst_cuda_context_push (data->nvenc->cuda_ctx);
  data_pointer = data->in_gl_resource->cuda_pointer;
  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (data->info); i++) {
    guint plane_n_components;
    GstGLBuffer *gl_buf_obj;
    GstGLMemoryPBO *gl_mem;
    guint src_stride, dest_stride;

    gl_mem =
        (GstGLMemoryPBO *) gst_buffer_peek_memory (data->frame->input_buffer,
        i);
    g_return_if_fail (gst_is_gl_memory_pbo ((GstMemory *) gl_mem));
    data->in_gl_resource->gl_mem[i] = GST_GL_MEMORY_CAST (gl_mem);
    plane_n_components = _plane_get_n_components (data->info, i);

    gl_buf_obj = (GstGLBuffer *) gl_mem->pbo;
    g_return_if_fail (gl_buf_obj != NULL);

    /* get the texture into the PBO */
    gst_gl_memory_pbo_upload_transfer (gl_mem);
    gst_gl_memory_pbo_download_transfer (gl_mem);

    GST_LOG_OBJECT (data->nvenc, "attempting to copy texture %u into cuda",
        gl_mem->mem.tex_id);

    if (!gst_cuda_result (CuGraphicsGLRegisterBuffer (&data->in_gl_resource->
                cuda_texture, gl_buf_obj->id,
                cudaGraphicsRegisterFlagsReadOnly))) {
      GST_ERROR_OBJECT (data->nvenc, "failed to register GL texture %u to cuda",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    if (!gst_cuda_result (CuGraphicsMapResources (1,
                &data->in_gl_resource->cuda_texture, 0))) {
      GST_ERROR_OBJECT (data->nvenc, "failed to map GL texture %u into cuda",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    if (!gst_cuda_result (CuGraphicsResourceGetMappedPointer
            (&data->in_gl_resource->cuda_plane_pointers[i],
                &data->in_gl_resource->cuda_num_bytes,
                data->in_gl_resource->cuda_texture))) {
      GST_ERROR_OBJECT (data->nvenc,
          "failed to get mapped pointer of map GL texture %u",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    src_stride = GST_VIDEO_INFO_PLANE_STRIDE (data->info, i);
    dest_stride = data->in_gl_resource->cuda_stride;

    /* copy into scratch buffer */
    param.srcXInBytes = 0;
    param.srcY = 0;
    param.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    param.srcDevice = data->in_gl_resource->cuda_plane_pointers[i];
    param.srcPitch = src_stride;

    param.dstXInBytes = 0;
    param.dstY = 0;
    param.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    param.dstDevice = (CUdeviceptr) data_pointer;
    param.dstPitch = dest_stride;
    param.WidthInBytes = _get_plane_width (data->info, i) * plane_n_components;
    param.Height = _get_plane_height (data->info, i);

    if (!gst_cuda_result (CuMemcpy2D (&param))) {
      GST_ERROR_OBJECT (data->nvenc, "failed to copy GL texture %u into cuda",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    if (!gst_cuda_result (CuGraphicsUnmapResources (1,
                &data->in_gl_resource->cuda_texture, 0))) {
      GST_ERROR_OBJECT (data->nvenc, "failed to unmap GL texture %u from cuda",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    if (!gst_cuda_result (CuGraphicsUnregisterResource (data->in_gl_resource->
                cuda_texture))) {
      GST_ERROR_OBJECT (data->nvenc,
          "failed to unregister GL texture %u from cuda", gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    data_pointer =
        data_pointer +
        data->in_gl_resource->cuda_stride *
        _get_plane_height (&data->nvenc->input_info, i);
  }
  gst_cuda_context_pop ();
}
#endif

static NvBaseEncFrameState *
_acquire_free_item (GstNvBaseEnc * nvenc)
{
  NvBaseEncFrameState *ret;

  GST_LOG_OBJECT (nvenc, "acquiring free state..");
  GST_VIDEO_ENCODER_STREAM_UNLOCK (nvenc);
  ret = g_async_queue_pop (nvenc->internal_pool);
  GST_VIDEO_ENCODER_STREAM_LOCK (nvenc);

  return ret;
}

static GstFlowReturn
_submit_input_buffer (GstNvBaseEnc * nvenc, GstVideoCodecFrame * frame,
    GstVideoFrame * vframe, gpointer inputBuffer, gpointer inputBufferPtr,
    NV_ENC_BUFFER_FORMAT bufferFormat, gpointer outputBufferPtr)
{
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (nvenc);
  NV_ENC_PIC_PARAMS pic_params = { 0, };
  NVENCSTATUS nv_ret;

  GST_LOG_OBJECT (nvenc, "%u: input buffer %p, output buffer %p, "
      "pts %" GST_TIME_FORMAT, frame->system_frame_number, inputBuffer,
      outputBufferPtr, GST_TIME_ARGS (frame->pts));

  pic_params.version = NV_ENC_PIC_PARAMS_VER;
  pic_params.inputBuffer = inputBufferPtr;
  pic_params.bufferFmt = bufferFormat;

  pic_params.inputWidth = GST_VIDEO_FRAME_WIDTH (vframe);
  pic_params.inputHeight = GST_VIDEO_FRAME_HEIGHT (vframe);
  pic_params.outputBitstream = outputBufferPtr;
  pic_params.completionEvent = NULL;
  if (GST_VIDEO_FRAME_IS_INTERLACED (vframe)) {
    if (GST_VIDEO_FRAME_IS_TFF (vframe))
      pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM;
    else
      pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;
  } else {
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  }
  pic_params.inputTimeStamp = frame->pts;
  pic_params.inputDuration =
      GST_CLOCK_TIME_IS_VALID (frame->duration) ? frame->duration : 0;
  pic_params.frameIdx = frame->system_frame_number;

  if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame))
    pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
  else
    pic_params.encodePicFlags = 0;

  if (nvenc_class->set_pic_params
      && !nvenc_class->set_pic_params (nvenc, frame, &pic_params)) {
    GST_ERROR_OBJECT (nvenc, "Subclass failed to submit buffer");
    return GST_FLOW_ERROR;
  }

  nv_ret = NvEncEncodePicture (nvenc->encoder, &pic_params);
  if (nv_ret == NV_ENC_SUCCESS) {
    GST_LOG_OBJECT (nvenc, "Encoded picture");
  } else if (nv_ret == NV_ENC_ERR_NEED_MORE_INPUT) {
    /* FIXME: we should probably queue pending output buffers here and only
     * submit them to the async queue once we got sucess back */
    GST_DEBUG_OBJECT (nvenc, "Encoded picture (encoder needs more input)");
  } else {
    GST_ERROR_OBJECT (nvenc, "Failed to encode picture: %d", nv_ret);

    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static gboolean
gst_nv_base_enc_upload_frame (GstNvBaseEnc * nvenc, GstVideoFrame * frame,
    NV_ENC_LOCK_INPUT_BUFFER * buf)
{
  gint offset[GST_VIDEO_MAX_PLANES];
  gint stride[GST_VIDEO_MAX_PLANES];
  GstVideoFormat format;
  gint i, j;

  format = GST_VIDEO_FRAME_FORMAT (frame);

  switch (format) {
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
      offset[0] = 0;
      stride[0] = buf->pitch;
      offset[1] =
          offset[0] + stride[0] * GST_VIDEO_FRAME_COMP_HEIGHT (frame, 0);
      stride[1] = buf->pitch / 2;
      offset[2] =
          offset[1] + stride[1] * GST_VIDEO_FRAME_COMP_HEIGHT (frame, 1);
      stride[2] = buf->pitch / 2;
      break;
    case GST_VIDEO_FORMAT_NV12:
      offset[0] = 0;
      stride[0] = buf->pitch;
      offset[1] =
          offset[0] + stride[0] * GST_VIDEO_FRAME_COMP_HEIGHT (frame, 0);
      stride[1] = buf->pitch;
      break;
    default:
      GST_ERROR_OBJECT (nvenc,
          "cannot support format %s", gst_video_format_to_string (format));
      return FALSE;
  }

  for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (frame); i++) {
    guint8 *src, *dst;
    guint height = GST_VIDEO_FRAME_COMP_HEIGHT (frame, i);
    guint width = GST_VIDEO_FRAME_COMP_WIDTH (frame, i) *
        GST_VIDEO_FRAME_COMP_PSTRIDE (frame, i);
    gint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, i);

    src = GST_VIDEO_FRAME_PLANE_DATA (frame, i);
    dst = ((guint8 *) buf->bufferDataPtr) + offset[i];

    for (j = 0; j < height; j++) {
      memcpy (dst, src, width);
      dst += stride[i];
      src += src_stride;
    }
  }

  return TRUE;
}

static GstFlowReturn
gst_nv_base_enc_handle_frame (GstVideoEncoder * enc, GstVideoCodecFrame * frame)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  NV_ENC_OUTPUT_PTR out_buf;
  NVENCSTATUS nv_ret;
  GstVideoFrame vframe;
  GstVideoInfo *info = &nvenc->input_state->info;
  GstFlowReturn flow = GST_FLOW_ERROR;
  GstMapFlags in_map_flags = GST_MAP_READ;
  NvBaseEncFrameState *state = NULL;

  g_assert (nvenc->encoder != NULL);

  if (g_atomic_int_compare_and_exchange (&nvenc->reconfig, TRUE, FALSE)) {
    if (!gst_nv_base_enc_set_format (enc, nvenc->input_state)) {
      flow = GST_FLOW_NOT_NEGOTIATED;
      goto drop;
    }
  }
#if HAVE_NVENC_GST_GL
  if (nvenc->input_type == GST_NVENC_INPUT_GL)
    in_map_flags |= GST_MAP_GL;
#endif

  if (nvenc->input_type == GST_NVENC_INPUT_CUDA)
    in_map_flags |= GST_MAP_CUDA;

  if (!gst_video_frame_map (&vframe, info, frame->input_buffer, in_map_flags)) {
    goto drop;
  }

  /* make sure our thread that waits for output to be ready is started */
  if (nvenc->bitstream_thread == NULL) {
    if (!gst_nv_base_enc_start_bitstream_thread (nvenc)) {
      gst_video_frame_unmap (&vframe);
      goto unmap_and_drop;
    }
  }

  state = _acquire_free_item (nvenc);

  if (state == NULL) {
    goto unmap_and_drop;
  }

  out_buf = state->out_buf;

  switch (nvenc->input_type) {
#if HAVE_NVENC_GST_GL
    case GST_NVENC_INPUT_GL:{
      NvBaseEncGLResource *in_gl_resource = state->in_buf;
      NvBaseEncGLMap data;

      GST_LOG_OBJECT (enc, "got input buffer %p", in_gl_resource);

      in_gl_resource->gl_mem[0] =
          (GstGLMemory *) gst_buffer_peek_memory (frame->input_buffer, 0);
      g_assert (gst_is_gl_memory ((GstMemory *) in_gl_resource->gl_mem[0]));

      data.nvenc = nvenc;
      data.frame = frame;
      data.info = &vframe.info;
      data.in_gl_resource = in_gl_resource;

      gst_gl_context_thread_add (in_gl_resource->gl_mem[0]->mem.context,
          (GstGLContextThreadFunc) _map_gl_input_buffer, &data);

      in_gl_resource->nv_mapped_resource.version =
          NV_ENC_MAP_INPUT_RESOURCE_VER;
      in_gl_resource->nv_mapped_resource.registeredResource =
          in_gl_resource->nv_resource.registeredResource;

      nv_ret =
          NvEncMapInputResource (nvenc->encoder,
          &in_gl_resource->nv_mapped_resource);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT (nvenc, "Failed to map input resource %p, ret %d",
            in_gl_resource, nv_ret);
        goto unmap_and_drop;
      }

      in_gl_resource->mapped = TRUE;

      flow =
          _submit_input_buffer (nvenc, frame, &vframe, in_gl_resource,
          in_gl_resource->nv_mapped_resource.mappedResource,
          in_gl_resource->nv_mapped_resource.mappedBufferFmt, out_buf);
      break;
    }
#endif
    case GST_NVENC_INPUT_CUDA:{
      NvBaseEncCudaResource *in_cuda_resource = state->in_buf;

      GST_LOG_OBJECT (enc, "got input buffer %p", in_cuda_resource);

      in_cuda_resource->nv_resource.version = NV_ENC_REGISTER_RESOURCE_VER;
      in_cuda_resource->nv_resource.resourceType =
          NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
      in_cuda_resource->nv_resource.width = GST_VIDEO_FRAME_WIDTH (&vframe);
      in_cuda_resource->nv_resource.height = GST_VIDEO_FRAME_HEIGHT (&vframe);
      in_cuda_resource->nv_resource.pitch =
          GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
      in_cuda_resource->nv_resource.bufferFormat =
          gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info));
      in_cuda_resource->nv_resource.resourceToRegister = vframe.map[0].data;

      gst_cuda_context_push (nvenc->cuda_ctx);
      if (!gst_cuda_result (NvEncRegisterResource (nvenc->encoder,
                  &in_cuda_resource->nv_resource))) {
        GST_ERROR_OBJECT (nvenc, "Failed to register CUDA resource");
        gst_cuda_context_pop ();
        goto unmap_and_drop;
      }

      in_cuda_resource->nv_mapped_resource.version =
          NV_ENC_MAP_INPUT_RESOURCE_VER;
      in_cuda_resource->nv_mapped_resource.registeredResource =
          in_cuda_resource->nv_resource.registeredResource;

      if (!gst_cuda_result (NvEncMapInputResource (nvenc->encoder,
                  &in_cuda_resource->nv_mapped_resource))) {
        GST_ERROR_OBJECT (nvenc, "Failed to map CUDA resource");
        gst_cuda_context_pop ();
        goto unmap_and_drop;
      }

      in_cuda_resource->buffer = gst_buffer_ref (frame->input_buffer);

      flow =
          _submit_input_buffer (nvenc, frame, &vframe, in_cuda_resource,
          in_cuda_resource->nv_mapped_resource.mappedResource,
          in_cuda_resource->nv_mapped_resource.mappedBufferFmt, out_buf);

      gst_cuda_context_pop ();
      break;
    }
    case GST_NVENC_INPUT_HOST:{
      NV_ENC_LOCK_INPUT_BUFFER in_buf_lock = { 0, };
      NV_ENC_INPUT_PTR in_buf = state->in_buf;

      GST_LOG_OBJECT (enc, "got input buffer %p", in_buf);

      in_buf_lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
      in_buf_lock.inputBuffer = in_buf;

      nv_ret = NvEncLockInputBuffer (nvenc->encoder, &in_buf_lock);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT (nvenc, "Failed to lock input buffer: %d", nv_ret);
        /* FIXME: post proper error message */
        goto unmap_and_drop;
      }
      GST_LOG_OBJECT (nvenc, "Locked input buffer %p", in_buf);

      if (!gst_nv_base_enc_upload_frame (nvenc, &vframe, &in_buf_lock)) {
        GST_ERROR_OBJECT (nvenc, "Failed to upload frame");
        goto unmap_and_drop;
      }

      nv_ret = NvEncUnlockInputBuffer (nvenc->encoder, in_buf);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT (nvenc, "Failed to unlock input buffer: %d", nv_ret);
        goto unmap_and_drop;
      }

      flow =
          _submit_input_buffer (nvenc, frame, &vframe, in_buf, in_buf,
          gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info)),
          out_buf);
      break;
    }
    default:
      g_assert_not_reached ();
      return GST_FLOW_ERROR;
  }

  if (flow != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (nvenc, "return state to pool");
    g_async_queue_push (nvenc->internal_pool, state);
    goto unmap_and_drop;
  }

  /* NvBaseEncFrameState shouldn't be freed by DestroyNotify */
  gst_video_codec_frame_set_user_data (frame, state, NULL);
  g_async_queue_push (nvenc->processing_queue, state);
  flow = g_atomic_int_get (&nvenc->last_flow);

  gst_video_frame_unmap (&vframe);
  /* encoder will keep frame in list internally, we'll look it up again later
   * in the thread where we get the output buffers and finish it there */
  gst_video_codec_frame_unref (frame);

  return flow;

/* ERRORS */
unmap_and_drop:
  {
    gst_video_frame_unmap (&vframe);
    goto drop;
  }
drop:
  {
    gst_video_encoder_finish_frame (enc, frame);
    return flow;
  }
}

static gboolean
gst_nv_base_enc_drain_encoder (GstNvBaseEnc * nvenc)
{
  NV_ENC_PIC_PARAMS pic_params = { 0, };
  NVENCSTATUS nv_ret;

  GST_INFO_OBJECT (nvenc, "draining encoder");

  if (nvenc->input_state == NULL) {
    GST_DEBUG_OBJECT (nvenc, "no input state, nothing to do");
    return TRUE;
  }

  pic_params.version = NV_ENC_PIC_PARAMS_VER;
  pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

  nv_ret = NvEncEncodePicture (nvenc->encoder, &pic_params);
  if (nv_ret != NV_ENC_SUCCESS) {
    GST_LOG_OBJECT (nvenc, "Failed to drain encoder, ret %d", nv_ret);
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_nv_base_enc_finish (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  if (!gst_nv_base_enc_drain_encoder (nvenc))
    return GST_FLOW_ERROR;

  gst_nv_base_enc_stop_bitstream_thread (nvenc, FALSE);

  return GST_FLOW_OK;
}

#if 0
static gboolean
gst_nv_base_enc_flush (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GST_INFO_OBJECT (nvenc, "done flushing encoder");
  return TRUE;
}
#endif

static void
gst_nv_base_enc_schedule_reconfig (GstNvBaseEnc * nvenc)
{
  g_atomic_int_set (&nvenc->reconfig, TRUE);
}

static void
gst_nv_base_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      nvenc->cuda_device_id = g_value_get_int (value);
      break;
    case PROP_PRESET:
      nvenc->preset_enum = g_value_get_enum (value);
      nvenc->selected_preset = _nv_preset_to_guid (nvenc->preset_enum);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_RC_MODE:
      nvenc->rate_control_mode = g_value_get_enum (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_MIN:
      nvenc->qp_min = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_MAX:
      nvenc->qp_max = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_CONST:
      nvenc->qp_const = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_BITRATE:
      nvenc->bitrate = g_value_get_uint (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_GOP_SIZE:
      nvenc->gop_size = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_RC_LOOKAHEAD:
      nvenc->rc_lookahead = g_value_get_uint (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_NO_SCENECUT:
      nvenc->no_scenecut = g_value_get_boolean (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_B_ADAPT:
      nvenc->b_adapt = g_value_get_boolean (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_BFRAMES:
      nvenc->bframes = g_value_get_uint (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_nv_base_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      g_value_set_int (value, nvenc->cuda_device_id);
      break;
    case PROP_PRESET:
      g_value_set_enum (value, nvenc->preset_enum);
      break;
    case PROP_RC_MODE:
      g_value_set_enum (value, nvenc->rate_control_mode);
      break;
    case PROP_QP_MIN:
      g_value_set_int (value, nvenc->qp_min);
      break;
    case PROP_QP_MAX:
      g_value_set_int (value, nvenc->qp_max);
      break;
    case PROP_QP_CONST:
      g_value_set_int (value, nvenc->qp_const);
      break;
    case PROP_BITRATE:
      g_value_set_uint (value, nvenc->bitrate);
      break;
    case PROP_GOP_SIZE:
      g_value_set_int (value, nvenc->gop_size);
      break;
    case PROP_RC_LOOKAHEAD:
      g_value_set_uint (value, nvenc->rc_lookahead);
      break;
    case PROP_NO_SCENECUT:
      g_value_set_boolean (value, nvenc->no_scenecut);
      break;
    case PROP_B_ADAPT:
      g_value_set_boolean (value, nvenc->b_adapt);
      break;
    case PROP_BFRAMES:
      g_value_set_uint (value, nvenc->bframes);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}