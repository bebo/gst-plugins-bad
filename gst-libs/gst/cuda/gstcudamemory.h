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

#ifndef __GST_CUDA_MEMORY_H__
#define __GST_CUDA_MEMORY_H__

#include <gst/gst.h>
#include <gst/gstallocator.h>
#include <gst/cuda/gstcuda_fwd.h>
#include <gst/cuda/cuda-prelude.h>

G_BEGIN_DECLS

#define GST_TYPE_CUDA_ALLOCATOR             (gst_cuda_allocator_get_type())
#define GST_CUDA_ALLOCATOR(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_CUDA_ALLOCATOR,GstCudaAllocator))
#define GST_CUDA_ALLOCATOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_CUDA_ALLOCATOR,GstCudaAllocatorClass))
#define GST_CUDA_ALLOCATOR_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), GST_TYPE_CUDA_ALLOCATOR,GstCudaAllocatorClass))
#define GST_IS_CUDA_ALLOCATOR(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_CUDA_ALLOCATOR))
#define GST_IS_CUDA_ALLOCATOR_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_CUDA_ALLOCATOR))
#define GST_CUDA_ALLOCATOR_CAST(obj)        ((GstCudaAllocator *)(obj))
#define GST_CUDA_MEMORY_CAST(mem)           ((GstCudaMemory *) (mem))

/**
 * GST_MAP_CUDA:
 *
 * Flag indicating that we should map the CUDA device memory
 * instead of to system memory.
 *
 * Combining #GST_MAP_CUDA with #GST_MAP_WRITE has the same semantics as though
 * you are writing to CUDA device/host memory.
 * Conversely, combining #GST_MAP_CUDA with
 * #GST_MAP_READ has the same semantics as though you are reading from
 * CUDA device/host memory
 */
#define GST_MAP_CUDA (GST_MAP_FLAG_LAST << 1)

#define GST_CUDA_MEMORY_TYPE_NAME "gst.cuda.memory"

/**
 * GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY:
 *
 * Name of the caps feature for indicating the use of #GstCudaMemory
 */
#define GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY "memory:CUDAMemory"

typedef enum
{
  GST_CUDA_MEMORY_TARGET_HOST,
  GST_CUDA_MEMORY_TARGET_DEVICE
} GstCudaMemoryTarget;

struct _GstCudaAllocator
{
  GstAllocator parent;
  GstCudaContext *context;

  /* default CUDA memory target */
  GstCudaMemoryTarget default_target;
};

struct _GstCudaAllocatorClass
{
  GstAllocatorClass parent_class;
};

GST_CUDA_API
GType          gst_cuda_allocator_get_type (void);

GST_CUDA_API
GstAllocator * gst_cuda_allocator_new (GstCudaContext * context,
                                       GstCudaMemoryTarget target);


struct _GstCudaMemory
{
  GstMemory       mem;

  /* CUdeviceptr or gpointer depending on target */
  gpointer data;
  gpointer alloc_data;
  GstCudaMemoryTarget target;

  /* valid only if target is device memory */
  gpointer map_data;
  gpointer map_alloc_data;

  gint map_count;
  gboolean need_upload;

  GMutex lock;
};

GST_CUDA_API
gboolean        gst_is_cuda_memory        (GstMemory * mem);

GST_CUDA_API
gboolean        gst_cuda_memory_get_target (GstCudaMemory * mem,
                                            GstCudaMemoryTarget * target);

G_END_DECLS

#endif /* __GST_CUDA_MEMORY_H__ */
