/* GStreamer
 * Copyright (C) <2018-2019> Seungha Yang <seungha.yang@navercorp.com>
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

#ifndef __GST_CUDA_UTILS_H__
#define __GST_CUDA_UTILS_H__

#include <gst/gst.h>
#include <gst/cuda/cuda-prelude.h>
#include <gst/cuda/gstcuda_fwd.h>

G_BEGIN_DECLS

GST_CUDA_API
gboolean        gst_cuda_result (gint result);

GST_CUDA_API
gboolean        gst_cuda_ensure_element_context (GstElement * element,
                                                 GstCudaContext ** cuda_ctx,
                                                 gint device_id);

GST_CUDA_API
gboolean        gst_cuda_handle_set_context     (GstElement * element,
                                                 GstContext * context,
                                                 GstCudaContext ** cuda_ctx,
                                                 gint device_id);

GST_CUDA_API
gboolean        gst_cuda_handle_context_query   (GstElement * element,
                                                 GstQuery * query,
                                                 GstCudaContext * cuda_ctx);

GST_CUDA_API
GstContext *    gst_context_new_cuda_context    (GstCudaContext * context);


G_END_DECLS

#endif /* __GST_CUDA_UTILS_H__ */
