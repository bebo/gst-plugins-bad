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

#ifndef __GST_CUDA_FWD_H__
#define __GST_CUDA_FWD_H__

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstCudaContext GstCudaContext;
typedef struct _GstCudaContextClass GstCudaContextClass;

typedef struct _GstCudaAllocator GstCudaAllocator;
typedef struct _GstCudaAllocatorClass GstCudaAllocatorClass;

typedef struct _GstCudaMemory GstCudaMemory;

typedef struct _GstCudaBufferPool GstCudaBufferPool;
typedef struct _GstCudaBufferPoolClass GstCudaBufferPoolClass;

G_END_DECLS

#endif /* __GST_CUDA_FWD_H__ */
