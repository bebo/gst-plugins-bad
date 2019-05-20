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

#include <gst/gst.h>
#include <gst/cuda/gstcuda.h>
#include <gst/cuda/gstcuda_private.h>

#ifdef HAVE_NVDEC
#include "gstnvdec.h"
#endif

#ifdef HAVE_NVENC
#include "gstnvenc.h"
#endif

#include "gstcudaupload.h"
#include "gstcudadownload.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ret = TRUE;

  if (!gst_cuda_load_library ())
    return FALSE;

#ifdef HAVE_NVDEC
  if (gst_cuvid_load_library ()) {
    ret &= gst_element_register (plugin, "nvdec", GST_RANK_PRIMARY,
        GST_TYPE_NVDEC);
  }
#endif

#ifdef HAVE_NVENC
  ret &= gst_nvenc_plugin_init (plugin);
#endif

  ret &= gst_element_register (plugin, "cudadownload", GST_RANK_NONE,
      GST_TYPE_CUDA_DOWNLOAD);
  ret &= gst_element_register (plugin, "cudaupload", GST_RANK_NONE,
      GST_TYPE_CUDA_UPLOAD);

  return ret;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, nvcodec,
    "GStreamer NVCODEC plugin", plugin_init, VERSION, "LGPL",
    GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
