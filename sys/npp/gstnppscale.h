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

#ifndef __GST_NPP_SCALE_H__
#define __GST_NPP_SCALE_H__

#include <gst/gst.h>
#include <gst/cuda/gstcuda.h>
#include <gst/cuda/gstcuda_private.h>

#include "gstnpploader.h"

G_BEGIN_DECLS

#define GST_TYPE_NPP_SCALE             (gst_npp_scale_get_type())
#define GST_NPP_SCALE(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NPP_SCALE,GstNppScale))
#define GST_NPP_SCALE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_NPP_SCALE,GstNppScaleClass))
#define GST_NPP_SCALE_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_NPP_SCALE,GstNppScaleClass))
#define GST_IS_NPP_SCALE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NPP_SCALE))
#define GST_IS_NPP_SCALE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_NPP_SCALE))

typedef struct _GstNppScale GstNppScale;
typedef struct _GstNppScaleClass GstNppScaleClass;
typedef struct _NppResizeInfo NppResizeInfo;
typedef struct _NppStageData  NppStageData;

struct _NppStageData
{
  gboolean need_process;
  gboolean final_stage;

  GstVideoInfo in_info;
  GstVideoInfo out_info;

  CUdeviceptr out_buf;
};

struct _GstNppScale
{
  GstCudaBaseFilter parent;

  NppiInterpolationMode npp_interp_mode;

  NppStageData stage[3];

  /* element properties */
  guint device_id;
  gint scale_mode;

  CUdeviceptr in_fallback;
  CUdeviceptr out_fallback;
};

struct _GstNppScaleClass
{
  GstCudaBaseFilterClass parent_class;
};

GType gst_npp_scale_get_type (void);

G_END_DECLS

#endif /* __GST_NPP_SCALE_H__ */
