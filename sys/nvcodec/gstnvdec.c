/*
 * Copyright (C) 2017 Ericsson AB. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnvdec.h"
#include "gstcudautils.h"

GST_DEBUG_CATEGORY_STATIC (gst_nvdec_debug_category);
#define GST_CAT_DEFAULT gst_nvdec_debug_category

static void
copy_video_frame_to_gl_textures (GstGLContext * context, gpointer * args);

typedef struct _GstNvDecCudaGraphicsResourceInfo
{
  GstGLContext *gl_context;
  GstCudaContext *cuda_context;
  CUgraphicsResource resource;
} GstNvDecCudaGraphicsResourceInfo;

static void
register_cuda_resource (GstGLContext * context, gpointer * args)
{
  GstMemory *mem = GST_MEMORY_CAST (args[0]);
  GstNvDecCudaGraphicsResourceInfo *cgr_info =
      (GstNvDecCudaGraphicsResourceInfo *) args[1];
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  guint texture_id;
  GstCudaContext *ctx = cgr_info->cuda_context;

  if (!gst_cuda_context_push (ctx))
    GST_WARNING ("failed to lock CUDA context");

  if (gst_memory_map (mem, &map_info, GST_MAP_READ | GST_MAP_GL)) {
    texture_id = *(guint *) map_info.data;

    if (!gst_cuda_result (CuGraphicsGLRegisterImage (&cgr_info->resource,
                texture_id, GL_TEXTURE_2D,
                CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD)))
      GST_WARNING ("failed to register texture with CUDA");

    gst_memory_unmap (mem, &map_info);
  } else
    GST_WARNING ("failed to map memory");

  if (!gst_cuda_context_pop ())
    GST_WARNING ("failed to unlock CUDA context");
}

static void
unregister_cuda_resource (GstGLContext * context,
    GstNvDecCudaGraphicsResourceInfo * cgr_info)
{
  GstCudaContext *ctx = cgr_info->cuda_context;

  if (!gst_cuda_context_push (ctx))
    GST_WARNING ("failed to lock CUDA context");

  if (!gst_cuda_result (CuGraphicsUnregisterResource ((const CUgraphicsResource)
              cgr_info->resource)))
    GST_WARNING ("failed to unregister resource");

  if (!gst_cuda_context_pop ())
    GST_WARNING ("failed to unlock CUDA context");
}

static void
free_cgr_info (GstNvDecCudaGraphicsResourceInfo * cgr_info)
{
  gst_gl_context_thread_add (cgr_info->gl_context,
      (GstGLContextThreadFunc) unregister_cuda_resource, cgr_info);
  gst_object_unref (cgr_info->gl_context);
  g_object_unref (cgr_info->cuda_context);
  g_slice_free (GstNvDecCudaGraphicsResourceInfo, cgr_info);
}

static CUgraphicsResource
ensure_cuda_graphics_resource (GstMemory * mem, GstCudaContext * cuda_context)
{
  static GQuark quark = 0;
  GstNvDecCudaGraphicsResourceInfo *cgr_info;
  gpointer args[2];

  if (!gst_is_gl_base_memory (mem)) {
    GST_WARNING ("memory is not GL base memory");
    return NULL;
  }

  if (!quark)
    quark = g_quark_from_static_string ("GstNvDecCudaGraphicsResourceInfo");

  cgr_info = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem), quark);
  if (!cgr_info) {
    cgr_info = g_slice_new (GstNvDecCudaGraphicsResourceInfo);
    cgr_info->gl_context =
        gst_object_ref (GST_GL_BASE_MEMORY_CAST (mem)->context);
    cgr_info->cuda_context = g_object_ref (cuda_context);
    args[0] = mem;
    args[1] = cgr_info;
    gst_gl_context_thread_add (cgr_info->gl_context,
        (GstGLContextThreadFunc) register_cuda_resource, args);
    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), quark, cgr_info,
        (GDestroyNotify) free_cgr_info);
  }

  return cgr_info->resource;
}

static gboolean gst_nvdec_open (GstVideoDecoder * decoder);
static gboolean gst_nvdec_start (GstVideoDecoder * decoder);
static gboolean gst_nvdec_stop (GstVideoDecoder * decoder);
static gboolean gst_nvdec_close (GstVideoDecoder * decoder);
static gboolean gst_nvdec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static GstFlowReturn gst_nvdec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static gboolean gst_nvdec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query);
static void gst_nvdec_set_context (GstElement * element, GstContext * context);
static gboolean gst_nvdec_src_query (GstVideoDecoder * decoder,
    GstQuery * query);
static gboolean gst_nvdec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_nvdec_drain (GstVideoDecoder * decoder);

static GstStaticPadTemplate gst_nvdec_sink_template =
    GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SINK_NAME,
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, stream-format=byte-stream, alignment=au; "
        "video/x-h265, stream-format=byte-stream, alignment=au; "
        "video/mpeg, mpegversion={ 1, 2, 4 }, systemstream=false; "
        "image/jpeg; video/x-vp8; video/x-vp9")
    );

static GstStaticPadTemplate gst_nvdec_src_template =
    GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SRC_NAME,
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("NV12") ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "NV12") ", texture-target=2D")
    );

G_DEFINE_TYPE_WITH_CODE (GstNvDec, gst_nvdec, GST_TYPE_VIDEO_DECODER,
    GST_DEBUG_CATEGORY_INIT (gst_nvdec_debug_category, "nvdec", 0,
        "Debug category for the nvdec element"));

static void
gst_nvdec_class_init (GstNvDecClass * klass)
{
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (element_class,
      &gst_nvdec_sink_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_nvdec_src_template);

  gst_element_class_set_static_metadata (element_class, "NVDEC video decoder",
      "Codec/Decoder/Video/Hardware", "NVDEC video decoder",
      "Ericsson AB, http://www.ericsson.com");

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_nvdec_open);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_nvdec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_nvdec_stop);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_nvdec_close);
  video_decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_nvdec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_nvdec_handle_frame);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_nvdec_decide_allocation);
  video_decoder_class->src_query = GST_DEBUG_FUNCPTR (gst_nvdec_src_query);
  video_decoder_class->drain = GST_DEBUG_FUNCPTR (gst_nvdec_drain);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_nvdec_flush);

  element_class->set_context = GST_DEBUG_FUNCPTR (gst_nvdec_set_context);
}

static void
gst_nvdec_init (GstNvDec * nvdec)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (nvdec), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (nvdec), TRUE);

  nvdec->last_ret = GST_FLOW_OK;
}

static gboolean
parser_sequence_callback (GstNvDec * nvdec, CUVIDEOFORMAT * format)
{
  guint width, height, fps_n, fps_d;
  CUVIDDECODECREATEINFO create_info = { 0, };
  gboolean ret = TRUE;
  GstCudaContext *ctx = nvdec->cuda_context;

  width = format->display_area.right - format->display_area.left;
  height = format->display_area.bottom - format->display_area.top;
  GST_DEBUG_OBJECT (nvdec, "width: %u, height: %u", width, height);

  if (!nvdec->decoder || (nvdec->width != width || nvdec->height != height)) {
    if (!gst_cuda_context_push (ctx)) {
      GST_ERROR_OBJECT (nvdec, "failed to lock CUDA context");
      return FALSE;
    }

    if (nvdec->decoder) {
      GST_DEBUG_OBJECT (nvdec, "destroying decoder");
      if (!gst_cuda_result (CuvidDestroyDecoder (nvdec->decoder))) {
        GST_ERROR_OBJECT (nvdec, "failed to destroy decoder");
        ret = FALSE;
      } else
        nvdec->decoder = NULL;
    }

    GST_DEBUG_OBJECT (nvdec, "creating decoder");
    create_info.ulWidth = width;
    create_info.ulHeight = height;
    create_info.ulNumDecodeSurfaces = 20;
    create_info.CodecType = format->codec;
    create_info.ChromaFormat = format->chroma_format;
    create_info.ulCreationFlags = cudaVideoCreate_Default;
    create_info.display_area.left = format->display_area.left;
    create_info.display_area.top = format->display_area.top;
    create_info.display_area.right = format->display_area.right;
    create_info.display_area.bottom = format->display_area.bottom;
    create_info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    create_info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    create_info.ulTargetWidth = width;
    create_info.ulTargetHeight = height;
    create_info.ulNumOutputSurfaces = 1;
    create_info.target_rect.left = 0;
    create_info.target_rect.top = 0;
    create_info.target_rect.right = width;
    create_info.target_rect.bottom = height;

    if (nvdec->decoder
        || !gst_cuda_result (CuvidCreateDecoder (&nvdec->decoder,
                &create_info))) {
      GST_ERROR_OBJECT (nvdec, "failed to create decoder");
      ret = FALSE;
    }

    if (!gst_cuda_context_pop ()) {
      GST_ERROR_OBJECT (nvdec, "failed to unlock CUDA context");
      ret = FALSE;
    }
  }

  fps_n = format->frame_rate.numerator;
  fps_d = MAX (1, format->frame_rate.denominator);

  if (!gst_pad_has_current_caps (GST_VIDEO_DECODER_SRC_PAD (nvdec))
      || width != nvdec->width || height != nvdec->height
      || fps_n != nvdec->fps_n || fps_d != nvdec->fps_d) {
    GstVideoCodecState *state;
    GstVideoInfo *vinfo;

    nvdec->width = width;
    nvdec->height = height;
    nvdec->fps_n = fps_n;
    nvdec->fps_d = fps_d;

    state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (nvdec),
        GST_VIDEO_FORMAT_NV12, nvdec->width, nvdec->height, nvdec->input_state);
    vinfo = &state->info;
    vinfo->fps_n = fps_n;
    vinfo->fps_d = fps_d;
    if (format->progressive_sequence) {
      vinfo->interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

      /* nvdec doesn't seem to deal with interlacing with hevc so rely
       * on upstream's value */
      if (format->codec == cudaVideoCodec_HEVC) {
        vinfo->interlace_mode = nvdec->input_state->info.interlace_mode;
      }
    } else {
      vinfo->interlace_mode = GST_VIDEO_INTERLACE_MODE_MIXED;
    }

    GST_LOG_OBJECT (nvdec,
        "Reading colorimetry information full-range %d matrix %d transfer %d primaries %d",
        format->video_signal_description.video_full_range_flag,
        format->video_signal_description.matrix_coefficients,
        format->video_signal_description.transfer_characteristics,
        format->video_signal_description.color_primaries);

    switch (format->video_signal_description.color_primaries) {
      case 1:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
        break;
      case 4:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT470M;
        break;
      case 5:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT470BG;
        break;
      case 6:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
        break;
      case 7:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE240M;
        break;
      case 8:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_FILM;
        break;
      case 9:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
        break;
      default:
        vinfo->colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;

    }

    if (format->video_signal_description.video_full_range_flag)
      vinfo->colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
    else
      vinfo->colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;

    switch (format->video_signal_description.transfer_characteristics) {
      case 1:
      case 6:
      case 16:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;
        break;
      case 4:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_GAMMA22;
        break;
      case 5:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_GAMMA28;
        break;
      case 7:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE240M;
        break;
      case 8:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_GAMMA10;
        break;
      case 9:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_LOG100;
        break;
      case 10:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_LOG316;
        break;
      case 15:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_BT2020_12;
        break;
      default:
        vinfo->colorimetry.transfer = GST_VIDEO_TRANSFER_UNKNOWN;
        break;
    }
    switch (format->video_signal_description.matrix_coefficients) {
      case 0:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
        break;
      case 1:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
        break;
      case 4:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_FCC;
        break;
      case 5:
      case 6:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
        break;
      case 7:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_SMPTE240M;
        break;
      case 9:
      case 10:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
        break;
      default:
        vinfo->colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;
        break;
    }

    state->caps = gst_video_info_to_caps (&state->info);

    {
      GstCaps *caps;
      caps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (nvdec));
      GST_DEBUG_OBJECT (nvdec, "Allowed caps %" GST_PTR_FORMAT, caps);

      nvdec->use_gl = FALSE;
      if (!caps || gst_caps_is_any (caps)) {
        GST_DEBUG_OBJECT (nvdec,
            "cannot determine output format, use system memory");
      } else {
        GstCapsFeatures *features;
        guint size = gst_caps_get_size (caps);
        guint i;

        for (i = 0; i < size; i++) {
          features = gst_caps_get_features (caps, i);
          if (features && gst_caps_features_contains (features,
                  GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
            GST_DEBUG_OBJECT (nvdec, "found GL memory feature, use gl");
            nvdec->use_gl = TRUE;
            break;
          }
        }
      }
      gst_clear_caps (&caps);
    }

    if (nvdec->use_gl) {
      gst_caps_set_features (state->caps, 0,
          gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
      gst_caps_set_simple (state->caps, "texture-target", G_TYPE_STRING,
          "2D", NULL);
    } else {
      GST_DEBUG_OBJECT (nvdec, "use system memory");
    }

    gst_video_codec_state_unref (state);

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (nvdec))) {
      GST_WARNING_OBJECT (nvdec, "failed to negotiate with downstream");
      nvdec->last_ret = GST_FLOW_NOT_NEGOTIATED;
      ret = FALSE;
    }
  }

  return ret;
}

static gboolean
parser_decode_callback (GstNvDec * nvdec, CUVIDPICPARAMS * params)
{
  GstCudaContext *ctx = nvdec->cuda_context;
  GList *iter, *pending_frames;

  GST_LOG_OBJECT (nvdec, "picture index: %u", params->CurrPicIdx);

  if (!gst_cuda_context_push (ctx))
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");

  if (!gst_cuda_result (CuvidDecodePicture (nvdec->decoder, params)))
    GST_WARNING_OBJECT (nvdec, "failed to decode picture");

  if (!gst_cuda_context_pop ())
    GST_WARNING_OBJECT (nvdec, "failed to unlock CUDA context");

  pending_frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (nvdec));

  /* HACK: this decode callback could be invoked multiple times for
   * one cuvidParseVideoData() call. Most likely it can be related to "decode only"
   * frame of VPX codec but no document available.
   * In that case, the last decoded frame seems to be displayed */
  for (iter = pending_frames; iter; iter = g_list_next (iter)) {
    guint id;
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) iter->data;
    gboolean set_data = FALSE;

    id = GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data (frame));
    if (G_UNLIKELY (nvdec->state == GST_NVDEC_STATE_DECODE)) {
      if (id) {
        GST_LOG_OBJECT (nvdec, "reset the last user data");
        set_data = TRUE;
      }
    } else if (!id) {
      set_data = TRUE;
    }

    if (set_data) {
      gst_video_codec_frame_set_user_data (frame,
          GUINT_TO_POINTER (params->CurrPicIdx + 1), NULL);
      break;
    }
  }

  nvdec->state = GST_NVDEC_STATE_DECODE;

  g_list_free_full (pending_frames,
      (GDestroyNotify) gst_video_codec_frame_unref);

  GST_LOG_OBJECT (nvdec, "decode callback done");

  return TRUE;
}

static gboolean
gst_nvdec_copy_frame_to_memory (GstNvDec * nvdec,
    CUVIDPARSERDISPINFO * dispinfo, GstVideoCodecFrame * frame)
{
  CUVIDPROCPARAMS params = { 0, };
  CUDA_MEMCPY2D copy_params = { 0, };
  CUdeviceptr dptr;
  guint pitch;
  GstVideoFrame video_frame;
  GstVideoCodecState *output_state;
  gint i;

  output_state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (nvdec));

  if (G_UNLIKELY (!output_state)) {
    GST_ERROR_OBJECT (nvdec, "output state is not set yet");
    return FALSE;
  }

  if (!gst_cuda_context_push (nvdec->cuda_context)) {
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");
    return FALSE;
  }

  if (!gst_video_frame_map (&video_frame, &output_state->info,
          frame->output_buffer, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (nvdec, "frame map failure");
    gst_video_codec_state_unref (output_state);
    gst_cuda_context_pop ();
    return FALSE;
  }

  params.progressive_frame = dispinfo->progressive_frame;
  params.second_field = dispinfo->repeat_first_field + 1;
  params.top_field_first = dispinfo->top_field_first;
  params.unpaired_field = dispinfo->repeat_first_field < 0;

  if (!gst_cuda_result (CuvidMapVideoFrame (nvdec->decoder,
              dispinfo->picture_index, &dptr, &pitch, &params))) {
    GST_ERROR_OBJECT (nvdec, "failed to map video frame");
    gst_video_codec_state_unref (output_state);
    gst_cuda_context_pop ();
    return FALSE;
  }

  copy_params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  copy_params.srcPitch = pitch;
  copy_params.dstMemoryType = CU_MEMORYTYPE_HOST;
  copy_params.WidthInBytes = nvdec->width;

  for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (&video_frame); i++) {
    copy_params.srcDevice = dptr + (i * pitch * nvdec->height);
    copy_params.dstHost = GST_VIDEO_FRAME_PLANE_DATA (&video_frame, i);
    copy_params.dstPitch = GST_VIDEO_FRAME_PLANE_STRIDE (&video_frame, i);
    copy_params.Height = nvdec->height >> (i ? 1 : 0);

    if (!gst_cuda_result (CuMemcpy2DAsync (&copy_params, 0))) {
      GST_ERROR_OBJECT (nvdec, "failed to copy %dth plane", i);
      CuvidUnmapVideoFrame (nvdec->decoder, dptr);
      gst_video_frame_unmap (&video_frame);
      gst_video_codec_state_unref (output_state);
      gst_cuda_context_pop ();
      return FALSE;
    }
  }

  if (!gst_cuda_result (CuStreamSynchronize (0))) {
    GST_ERROR_OBJECT (nvdec, "failed sync copy operation");
    CuvidUnmapVideoFrame (nvdec->decoder, dptr);
    gst_video_frame_unmap (&video_frame);
    gst_video_codec_state_unref (output_state);
    gst_cuda_context_pop ();
    return FALSE;
  }

  gst_video_codec_state_unref (output_state);
  gst_video_frame_unmap (&video_frame);

  if (!gst_cuda_result (CuvidUnmapVideoFrame (nvdec->decoder, dptr)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap video frame");

  if (!gst_cuda_context_pop ())
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");

  return TRUE;
}

static gboolean
parser_display_callback (GstNvDec * nvdec, CUVIDPARSERDISPINFO * dispinfo)
{
  GList *iter, *pending_frames;
  GstVideoCodecFrame *frame = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  guint num_resources, i;
  CUgraphicsResource *resources;
  gpointer args[4];
  GstMemory *mem;

  GST_LOG_OBJECT (nvdec, "picture index: %u", dispinfo->picture_index);

  pending_frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (nvdec));
  for (iter = pending_frames; iter; iter = g_list_next (iter)) {
    guint id;
    GstVideoCodecFrame *tmp = (GstVideoCodecFrame *) iter->data;

    id = GPOINTER_TO_UINT (gst_video_codec_frame_get_user_data (tmp));
    if (id == dispinfo->picture_index + 1) {
      frame = tmp;
      break;
    }
  }

  if (G_UNLIKELY (frame == NULL)) {
    GST_WARNING_OBJECT (nvdec, "no frame for picture index %u",
        dispinfo->picture_index);
    frame = g_slice_new0 (GstVideoCodecFrame);
    frame->ref_count = 1;
  }

  /* let's believe decoder's pts */
  frame->pts = dispinfo->timestamp;

  ret = gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (nvdec),
      frame);
  if (ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (nvdec, "failed to allocate output frame");
    nvdec->last_ret = ret;
    return FALSE;
  }

  if (!nvdec->use_gl) {
    gst_nvdec_copy_frame_to_memory (nvdec, dispinfo, frame);
    goto done;
  }

  num_resources = gst_buffer_n_memory (frame->output_buffer);
  resources = g_new (CUgraphicsResource, num_resources);

  for (i = 0; i < num_resources; i++) {
    mem = gst_buffer_get_memory (frame->output_buffer, i);
    resources[i] = ensure_cuda_graphics_resource (mem, nvdec->cuda_context);
    GST_MINI_OBJECT_FLAG_SET (mem, GST_GL_BASE_MEMORY_TRANSFER_NEED_DOWNLOAD);
    gst_memory_unref (mem);
  }

  args[0] = nvdec;
  args[1] = dispinfo;
  args[2] = resources;
  args[3] = GUINT_TO_POINTER (num_resources);
  gst_gl_context_thread_add (nvdec->gl_context,
      (GstGLContextThreadFunc) copy_video_frame_to_gl_textures, args);
  g_free (resources);

done:
  if (!dispinfo->progressive_frame) {
    GST_BUFFER_FLAG_SET (frame->output_buffer,
        GST_VIDEO_BUFFER_FLAG_INTERLACED);

    if (dispinfo->top_field_first) {
      GST_BUFFER_FLAG_SET (frame->output_buffer, GST_VIDEO_BUFFER_FLAG_TFF);
    }
    if (dispinfo->repeat_first_field == -1) {
      GST_BUFFER_FLAG_SET (frame->output_buffer,
          GST_VIDEO_BUFFER_FLAG_ONEFIELD);
    } else {
      GST_BUFFER_FLAG_SET (frame->output_buffer, GST_VIDEO_BUFFER_FLAG_RFF);
    }
  }

  pending_frames = g_list_remove (pending_frames, frame);
  g_list_free_full (pending_frames,
      (GDestroyNotify) gst_video_codec_frame_unref);

  ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (nvdec), frame);
  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (nvdec, "failed to finish frame %s",
        gst_flow_get_name (ret));
    nvdec->last_ret = ret;
  }

  GST_LOG_OBJECT (nvdec, "display callback done");

  return TRUE;
}

static gboolean
gst_nvdec_open (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  GST_DEBUG_OBJECT (nvdec, "open");

  /* FIXME: set device id */
  if (!gst_cuda_ensure_element_context (GST_ELEMENT_CAST (decoder),
          &nvdec->cuda_context, 0)) {
    GST_ERROR_OBJECT (nvdec, "failed to create CUDA context");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_nvdec_start (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  nvdec->state = GST_NVDEC_STATE_INIT;
  if (!nvdec->cuda_context) {
    GST_ERROR_OBJECT (nvdec, "No available CUDA context");
    return FALSE;
  }

  return TRUE;
}

static gboolean
maybe_destroy_decoder_and_parser (GstNvDec * nvdec)
{
  gboolean ret = TRUE;

  if (!gst_cuda_context_push (nvdec->cuda_context)) {
    GST_ERROR_OBJECT (nvdec, "failed to lock CUDA context");
    return FALSE;
  }

  if (nvdec->decoder) {
    GST_DEBUG_OBJECT (nvdec, "destroying decoder");
    ret = gst_cuda_result (CuvidDestroyDecoder (nvdec->decoder));
    if (ret)
      nvdec->decoder = NULL;
    else
      GST_ERROR_OBJECT (nvdec, "failed to destroy decoder");
  }

  if (!gst_cuda_context_pop ()) {
    GST_ERROR_OBJECT (nvdec, "failed to unlock CUDA context");
    return FALSE;
  }

  if (nvdec->parser) {
    GST_DEBUG_OBJECT (nvdec, "destroying parser");
    if (!gst_cuda_result (CuvidDestroyVideoParser (nvdec->parser))) {
      GST_ERROR_OBJECT (nvdec, "failed to destroy parser");
      return FALSE;
    }
    nvdec->parser = NULL;
  }

  return ret;
}

static gboolean
gst_nvdec_stop (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  GST_DEBUG_OBJECT (nvdec, "stop");

  nvdec->state = GST_NVDEC_STATE_INIT;
  if (!maybe_destroy_decoder_and_parser (nvdec))
    return FALSE;

  if (nvdec->cuda_context) {
    g_object_unref (nvdec->cuda_context);
    nvdec->cuda_context = NULL;
  }

  if (nvdec->gl_context) {
    gst_object_unref (nvdec->gl_context);
    nvdec->gl_context = NULL;
  }

  if (nvdec->other_gl_context) {
    gst_object_unref (nvdec->other_gl_context);
    nvdec->other_gl_context = NULL;
  }

  if (nvdec->gl_display) {
    gst_object_unref (nvdec->gl_display);
    nvdec->gl_display = NULL;
  }

  if (nvdec->input_state) {
    gst_video_codec_state_unref (nvdec->input_state);
    nvdec->input_state = NULL;
  }

  return TRUE;
}

static gboolean
gst_nvdec_close (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  GST_DEBUG_OBJECT (nvdec, "close");

  gst_clear_object (&nvdec->cuda_context);

  return TRUE;
}

static gboolean
gst_nvdec_set_format (GstVideoDecoder * decoder, GstVideoCodecState * state)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstStructure *s;
  const gchar *caps_name;
  gint mpegversion = 0;
  CUVIDPARSERPARAMS parser_params = { 0, };

  GST_DEBUG_OBJECT (nvdec, "set format");

  if (nvdec->input_state)
    gst_video_codec_state_unref (nvdec->input_state);

  nvdec->input_state = gst_video_codec_state_ref (state);

  if (!maybe_destroy_decoder_and_parser (nvdec))
    return FALSE;

  s = gst_caps_get_structure (state->caps, 0);
  caps_name = gst_structure_get_name (s);
  GST_DEBUG_OBJECT (nvdec, "codec is %s", caps_name);

  if (!g_strcmp0 (caps_name, "video/mpeg")) {
    if (gst_structure_get_int (s, "mpegversion", &mpegversion)) {
      switch (mpegversion) {
        case 1:
          parser_params.CodecType = cudaVideoCodec_MPEG1;
          break;
        case 2:
          parser_params.CodecType = cudaVideoCodec_MPEG2;
          break;
        case 4:
          parser_params.CodecType = cudaVideoCodec_MPEG4;
          break;
      }
    }
    if (!mpegversion) {
      GST_ERROR_OBJECT (nvdec, "could not get MPEG version");
      return FALSE;
    }
  } else if (!g_strcmp0 (caps_name, "video/x-h264")) {
    parser_params.CodecType = cudaVideoCodec_H264;
  } else if (!g_strcmp0 (caps_name, "image/jpeg")) {
    parser_params.CodecType = cudaVideoCodec_JPEG;
  } else if (!g_strcmp0 (caps_name, "video/x-h265")) {
    parser_params.CodecType = cudaVideoCodec_HEVC;
  } else if (!g_strcmp0 (caps_name, "video/x-vp8")) {
    parser_params.CodecType = cudaVideoCodec_VP8;
  } else if (!g_strcmp0 (caps_name, "video/x-vp9")) {
    parser_params.CodecType = cudaVideoCodec_VP9;
  } else {
    GST_ERROR_OBJECT (nvdec, "failed to determine codec type");
    return FALSE;
  }

  parser_params.ulMaxNumDecodeSurfaces = 20;
  parser_params.ulErrorThreshold = 100;
  parser_params.ulMaxDisplayDelay = 0;
  parser_params.ulClockRate = GST_SECOND;
  parser_params.pUserData = nvdec;
  parser_params.pfnSequenceCallback =
      (PFNVIDSEQUENCECALLBACK) parser_sequence_callback;
  parser_params.pfnDecodePicture =
      (PFNVIDDECODECALLBACK) parser_decode_callback;
  parser_params.pfnDisplayPicture =
      (PFNVIDDISPLAYCALLBACK) parser_display_callback;

  GST_DEBUG_OBJECT (nvdec, "creating parser");
  if (!gst_cuda_result (CuvidCreateVideoParser (&nvdec->parser,
              &parser_params))) {
    GST_ERROR_OBJECT (nvdec, "failed to create parser");
    return FALSE;
  }

  return TRUE;
}

static void
copy_video_frame_to_gl_textures (GstGLContext * context, gpointer * args)
{
  GstNvDec *nvdec = GST_NVDEC (args[0]);
  CUVIDPARSERDISPINFO *dispinfo = (CUVIDPARSERDISPINFO *) args[1];
  CUgraphicsResource *resources = (CUgraphicsResource *) args[2];
  guint num_resources = GPOINTER_TO_UINT (args[3]);
  CUVIDPROCPARAMS proc_params = { 0, };
  CUdeviceptr dptr;
  CUarray array;
  guint pitch, i;
  CUDA_MEMCPY2D mcpy2d = { 0, };

  GST_LOG_OBJECT (nvdec, "picture index: %u", dispinfo->picture_index);

  proc_params.progressive_frame = dispinfo->progressive_frame;
  proc_params.top_field_first = dispinfo->top_field_first;
  proc_params.unpaired_field = dispinfo->repeat_first_field == -1;

  if (!gst_cuda_context_push (nvdec->cuda_context)) {
    GST_WARNING_OBJECT (nvdec, "failed to lock CUDA context");
    return;
  }

  if (!gst_cuda_result (CuvidMapVideoFrame (nvdec->decoder,
              dispinfo->picture_index, &dptr, &pitch, &proc_params))) {
    GST_WARNING_OBJECT (nvdec, "failed to map CUDA video frame");
    goto unlock_cuda_context;
  }

  if (!gst_cuda_result (CuGraphicsMapResources (num_resources, resources,
              NULL))) {
    GST_WARNING_OBJECT (nvdec, "failed to map CUDA resources");
    goto unmap_video_frame;
  }

  mcpy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  mcpy2d.srcPitch = pitch;
  mcpy2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
  mcpy2d.dstPitch = nvdec->width;
  mcpy2d.WidthInBytes = nvdec->width;

  for (i = 0; i < num_resources; i++) {
    if (!gst_cuda_result (CuGraphicsSubResourceGetMappedArray (&array,
                resources[i], 0, 0))) {
      GST_WARNING_OBJECT (nvdec, "failed to map CUDA array");
      break;
    }

    mcpy2d.srcDevice = dptr + (i * pitch * nvdec->height);
    mcpy2d.dstArray = array;
    mcpy2d.Height = nvdec->height / (i + 1);

    if (!gst_cuda_result (CuMemcpy2D (&mcpy2d)))
      GST_WARNING_OBJECT (nvdec, "memcpy to mapped array failed");
  }

  if (!gst_cuda_result (CuGraphicsUnmapResources (num_resources, resources,
              NULL)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap CUDA resources");

unmap_video_frame:
  if (!gst_cuda_result (CuvidUnmapVideoFrame (nvdec->decoder, dptr)))
    GST_WARNING_OBJECT (nvdec, "failed to unmap CUDA video frame");

unlock_cuda_context:
  if (!gst_cuda_context_pop ())
    GST_WARNING_OBJECT (nvdec, "failed to unlock CUDA context");
}

static GstFlowReturn
gst_nvdec_handle_frame (GstVideoDecoder * decoder, GstVideoCodecFrame * frame)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  CUVIDSOURCEDATAPACKET packet = { 0, };

  GST_LOG_OBJECT (nvdec, "handle frame");

  /* initialize with zero to keep track of frames */
  gst_video_codec_frame_set_user_data (frame, GUINT_TO_POINTER (0), NULL);

  if (!gst_buffer_map (frame->input_buffer, &map_info, GST_MAP_READ)) {
    GST_ERROR_OBJECT (nvdec, "failed to map input buffer");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

  if (nvdec->last_ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (nvdec,
        "return last flow %s", gst_flow_get_name (nvdec->last_ret));
    return nvdec->last_ret;
  }

  packet.payload_size = (gulong) map_info.size;
  packet.payload = map_info.data;
  packet.timestamp = frame->pts;
  packet.flags = CUVID_PKT_TIMESTAMP;

  if (GST_BUFFER_IS_DISCONT (frame->input_buffer))
    packet.flags |= CUVID_PKT_DISCONTINUITY;

  nvdec->state = GST_NVDEC_STATE_PARSE;

  if (!gst_cuda_result (CuvidParseVideoData (nvdec->parser, &packet)))
    GST_WARNING_OBJECT (nvdec, "parser failed");

  gst_buffer_unmap (frame->input_buffer, &map_info);
  gst_video_codec_frame_unref (frame);

  return nvdec->last_ret;
}

static gboolean
gst_nvdec_flush (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  CUVIDSOURCEDATAPACKET packet = { 0, };

  GST_DEBUG_OBJECT (nvdec, "flush");

  packet.payload_size = 0;
  packet.payload = NULL;
  packet.flags = CUVID_PKT_ENDOFSTREAM;

  nvdec->state = GST_NVDEC_STATE_PARSE;
  if (nvdec->parser &&
      !gst_cuda_result (CuvidParseVideoData (nvdec->parser, &packet))) {
    GST_WARNING_OBJECT (nvdec, "parser failed");
  }

  nvdec->last_ret = GST_FLOW_OK;

  return TRUE;
}

static GstFlowReturn
gst_nvdec_drain (GstVideoDecoder * decoder)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  CUVIDSOURCEDATAPACKET packet = { 0, };

  GST_DEBUG_OBJECT (nvdec, "draining decoder");

  packet.payload_size = 0;
  packet.payload = NULL;
  packet.flags = CUVID_PKT_ENDOFSTREAM;

  nvdec->state = GST_NVDEC_STATE_PARSE;
  if (nvdec->parser &&
      !gst_cuda_result (CuvidParseVideoData (nvdec->parser, &packet))) {
    GST_WARNING_OBJECT (nvdec, "parser failed");
  }

  return nvdec->last_ret;
}

static gboolean
gst_nvdec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);
  GstCaps *outcaps;
  GstBufferPool *pool = NULL;
  guint n, size, min, max;
  GstVideoInfo vinfo = { 0, };
  GstStructure *config;

  GST_DEBUG_OBJECT (nvdec, "decide allocation");

  if (!nvdec->use_gl) {
    GST_FIXME_OBJECT (nvdec, "need to support cuda device memory pool");
    goto done;
  }

  if (!gst_gl_ensure_element_data (nvdec, &nvdec->gl_display,
          &nvdec->other_gl_context)) {
    GST_ERROR_OBJECT (nvdec, "failed to ensure OpenGL display");
    return FALSE;
  }

  if (!gst_gl_query_local_gl_context (GST_ELEMENT (decoder), GST_PAD_SRC,
          &nvdec->gl_context)) {
    GST_INFO_OBJECT (nvdec, "failed to query local OpenGL context");
    if (nvdec->gl_context)
      gst_object_unref (nvdec->gl_context);
    nvdec->gl_context =
        gst_gl_display_get_gl_context_for_thread (nvdec->gl_display, NULL);
    if (!nvdec->gl_context
        || !gst_gl_display_add_context (nvdec->gl_display, nvdec->gl_context)) {
      if (nvdec->gl_context)
        gst_object_unref (nvdec->gl_context);
      if (!gst_gl_display_create_context (nvdec->gl_display,
              nvdec->other_gl_context, &nvdec->gl_context, NULL)) {
        GST_ERROR_OBJECT (nvdec, "failed to create OpenGL context");
        return FALSE;
      }
      if (!gst_gl_display_add_context (nvdec->gl_display, nvdec->gl_context)) {
        GST_ERROR_OBJECT (nvdec,
            "failed to add the OpenGL context to the display");
        return FALSE;
      }
    }
  }

  gst_query_parse_allocation (query, &outcaps, NULL);
  n = gst_query_get_n_allocation_pools (query);
  if (n > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    if (!GST_IS_GL_BUFFER_POOL (pool)) {
      gst_object_unref (pool);
      pool = NULL;
    }
  }

  if (!pool) {
    pool = gst_gl_buffer_pool_new (nvdec->gl_context);

    if (outcaps)
      gst_video_info_from_caps (&vinfo, outcaps);
    size = (guint) vinfo.size;
    min = max = 0;
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_set_config (pool, config);
  if (n > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);
  gst_object_unref (pool);

done:
  return GST_VIDEO_DECODER_CLASS (gst_nvdec_parent_class)->decide_allocation
      (decoder, query);
}

static gboolean
gst_nvdec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstNvDec *nvdec = GST_NVDEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      if (gst_gl_handle_context_query (GST_ELEMENT (decoder), query,
              nvdec->gl_display, nvdec->gl_context, nvdec->other_gl_context))
        return TRUE;
      else if (gst_cuda_handle_context_query (GST_ELEMENT (decoder),
              query, nvdec->cuda_context))
        return TRUE;
      break;
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (gst_nvdec_parent_class)->src_query (decoder,
      query);
}

static void
gst_nvdec_set_context (GstElement * element, GstContext * context)
{
  GstNvDec *nvdec = GST_NVDEC (element);
  GST_DEBUG_OBJECT (nvdec, "set context %s",
      gst_context_get_context_type (context));

  /* FIXME: Add device-id */
  if (gst_cuda_handle_set_context (element, context, &nvdec->cuda_context, 0)) {
    goto done;
  }

  gst_gl_handle_set_context (element, context, &nvdec->gl_display,
      &nvdec->other_gl_context);

done:
  GST_ELEMENT_CLASS (gst_nvdec_parent_class)->set_context (element, context);
}
