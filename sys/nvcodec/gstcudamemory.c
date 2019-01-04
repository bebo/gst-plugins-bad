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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstcudamemory.h"
#include "gstcudautils.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (cudaallocator_debug);
#define GST_CAT_DEFAULT cudaallocator_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_MEMORY);

#define gst_cuda_allocator_parent_class parent_class
G_DEFINE_TYPE (GstCudaAllocator, gst_cuda_allocator, GST_TYPE_ALLOCATOR);

static void gst_cuda_allocator_dispose (GObject * object);
static GstMemory *gst_cuda_allocator_alloc (GstAllocator * allocator,
    gsize size, GstAllocationParams * params);
static void gst_cuda_allocator_free (GstAllocator * allocator,
    GstMemory * memory);

static gpointer cuda_mem_map (GstCudaMemory * mem, gsize maxsize,
    GstMapFlags flags);
static void cuda_mem_unmap (GstCudaMemory * mem);
static GstMemory *cuda_mem_share (GstCudaMemory * mem, gssize offset,
    gssize size);
static gboolean cuda_mem_is_span (GstCudaMemory * mem1, GstCudaMemory * mem2,
    gsize * offset);

static void
gst_cuda_allocator_class_init (GstCudaAllocatorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);

  gobject_class->dispose = gst_cuda_allocator_dispose;

  allocator_class->alloc = GST_DEBUG_FUNCPTR (gst_cuda_allocator_alloc);
  allocator_class->free = GST_DEBUG_FUNCPTR (gst_cuda_allocator_free);

  GST_DEBUG_CATEGORY_INIT (cudaallocator_debug, "cudaallocator", 0,
      "CUDA Allocator");
  GST_DEBUG_CATEGORY_GET (GST_CAT_MEMORY, "GST_MEMORY");
}

static void
gst_cuda_allocator_init (GstCudaAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  GST_DEBUG_OBJECT (allocator, "init");

  alloc->mem_type = GST_CUDA_MEMORY_TYPE_NAME;

  alloc->mem_map = (GstMemoryMapFunction) cuda_mem_map;
  alloc->mem_unmap = (GstMemoryUnmapFunction) cuda_mem_unmap;
  alloc->mem_share = (GstMemoryShareFunction) cuda_mem_share;
  alloc->mem_is_span = (GstMemoryIsSpanFunction) cuda_mem_is_span;
  /* Use the default, fallback copy function */

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static void
gst_cuda_allocator_dispose (GObject * object)
{
  GstCudaAllocator *self = GST_CUDA_ALLOCATOR_CAST (object);

  GST_DEBUG_OBJECT (self, "dispose");

  gst_clear_object (&self->context);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static GstCudaMemory *
gst_cuda_allocator_memory_new (GstCudaAllocator * self,
    GstMemoryFlags flags, gsize maxsize, gsize align, gsize offset, gsize size,
    GstCudaMemoryTarget target)
{
  gpointer data;
  gsize padding;
  gboolean ret = FALSE;
  GstCudaMemory *mem;

  if (!gst_cuda_context_push (self->context))
    return NULL;

  /* ensure configured alignment */
  align |= gst_memory_alignment;
  /* allocate more to compensate for alignment */
  maxsize += align;

  GST_CAT_DEBUG_OBJECT (GST_CAT_MEMORY, self,
      "allocate new cuda memory with target %d", target);

  switch (target) {
    case GST_CUDA_MEMORY_TARGET_HOST:
      ret = gst_cuda_result (CuMemAllocHost (&data, maxsize));
      break;
    case GST_CUDA_MEMORY_TARGET_DEVICE:
      ret = gst_cuda_result (CuMemAlloc ((CUdeviceptr *) & data, maxsize));
      break;
    default:
      GST_CAT_ERROR_OBJECT (GST_CAT_MEMORY, self,
          "unknown CUDA memory type %d", target);
      return NULL;
  }

  if (G_UNLIKELY (!ret)) {
    GST_CAT_ERROR_OBJECT (GST_CAT_MEMORY, self,
        "CUDA allocation failure for target %d", target);
    return NULL;
  }

  mem = g_slice_new0 (GstCudaMemory);
  mem->alloc_data = data;
  mem->target = target;
  g_mutex_init (&mem->lock);

  /* alignment makes sense only for host memory  */
  if (target == GST_CUDA_MEMORY_TARGET_HOST) {
    gsize aoffset;
    guint8 *align_data;

    align_data = data;

    if ((aoffset = ((guintptr) align_data & align))) {
      aoffset = (align + 1) - aoffset;
      align_data += aoffset;
      maxsize -= aoffset;
    }

    if (offset && (flags & GST_MEMORY_FLAG_ZERO_PREFIXED))
      memset (align_data, 0, offset);

    padding = maxsize - (offset + size);
    if (padding && (flags & GST_MEMORY_FLAG_ZERO_PADDED))
      memset (align_data + offset + size, 0, padding);

    mem->data = align_data;
  } else {
    mem->data = data;
  }

  gst_cuda_context_pop ();

  gst_memory_init (GST_MEMORY_CAST (mem),
      flags, GST_ALLOCATOR_CAST (self), NULL, maxsize, align, offset, size);

  return mem;
}

static GstMemory *
gst_cuda_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  GstCudaAllocator *self = GST_CUDA_ALLOCATOR_CAST (allocator);
  gsize maxsize = size + params->prefix + params->padding;

  return (GstMemory *) gst_cuda_allocator_memory_new (self,
      params->flags, maxsize, params->align, params->prefix, size,
      self->default_target);
}

static void
gst_cuda_allocator_free (GstAllocator * allocator, GstMemory * memory)
{
  GstCudaAllocator *self = GST_CUDA_ALLOCATOR_CAST (allocator);
  GstCudaMemory *mem = GST_CUDA_MEMORY_CAST (memory);

  GST_CAT_DEBUG_OBJECT (GST_CAT_MEMORY, allocator, "free cuda memory");

  g_mutex_clear (&mem->lock);

  gst_cuda_context_push (self->context);
  if (mem->alloc_data) {
    switch (mem->target) {
      case GST_CUDA_MEMORY_TARGET_HOST:
        gst_cuda_result (CuMemFreeHost (mem->alloc_data));
        break;
      case GST_CUDA_MEMORY_TARGET_DEVICE:
        gst_cuda_result (CuMemFree ((CUdeviceptr) mem->alloc_data));
        break;
      default:
        GST_CAT_ERROR_OBJECT (GST_CAT_MEMORY, allocator,
            "invalid CUDA memory type %d", mem->target);
        break;
    }
  }
  gst_cuda_context_pop ();

  g_slice_free (GstCudaMemory, mem);
}

static gpointer
gst_cuda_memory_device_memory_map (GstCudaMemory * mem)
{
  GstMemory *memory = GST_MEMORY_CAST (mem);
  gpointer data;
  gsize maxsize;
  gsize aoffset;
  guint8 *align_data;
  gsize align = memory->align;
  GstCudaAllocator *allocator = GST_CUDA_ALLOCATOR_CAST (memory->allocator);
  GstCudaContext *ctx = allocator->context;

  if (mem->map_data) {
    return mem->map_data;
  }

  GST_CAT_DEBUG (GST_CAT_MEMORY, "alloc host memory for map");

  if (!gst_cuda_context_push (ctx)) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot push cuda context");
    g_mutex_unlock (&mem->lock);
    return NULL;
  }

  maxsize = memory->maxsize + align;
  if (!gst_cuda_result (CuMemAllocHost (&data, maxsize))) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot alloc host memory");
    gst_cuda_context_pop ();
    return NULL;
  }

  mem->map_alloc_data = data;
  align_data = data;

  /* do align */
  if ((aoffset = ((guintptr) align_data & align))) {
    aoffset = (align + 1) - aoffset;
    align_data += aoffset;
  }
  mem->map_data = align_data;

  if (!gst_cuda_result (CuMemcpyDtoH (mem->map_data, (CUdeviceptr) mem->data,
              GST_MEMORY_CAST (mem)->maxsize))) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot copy device memory to host memory");
    CuMemFreeHost (data);
    mem->map_alloc_data = mem->map_data = NULL;
  }
  gst_cuda_context_pop ();

  return mem->map_data;
}

static gpointer
cuda_mem_map (GstCudaMemory * mem, gsize maxsize, GstMapFlags flags)
{
  if (mem->target == GST_CUDA_MEMORY_TARGET_HOST) {
    /* host memory is accessible without any requirement */
    return mem->data;
  } else if (mem->target == GST_CUDA_MEMORY_TARGET_DEVICE) {
    gpointer ret = NULL;

    g_mutex_lock (&mem->lock);
    /* caller expect CUDA memory */
    mem->map_count++;

    if ((flags & GST_MAP_CUDA) == GST_MAP_CUDA) {
      g_mutex_unlock (&mem->lock);
      return mem->data;
    }

    ret = gst_cuda_memory_device_memory_map (mem);
    if (ret == NULL) {
      mem->map_count--;
      g_mutex_unlock (&mem->lock);
      return NULL;
    }

    if (flags & GST_MAP_WRITE)
      mem->need_upload = TRUE;

    g_mutex_unlock (&mem->lock);
    return ret;
  } else {
    GST_CAT_ERROR (GST_CAT_MEMORY, "unknown memory target %d", mem->target);
  }

  return NULL;
}

static void
cuda_mem_unmap (GstCudaMemory * mem)
{
  GstCudaAllocator *allocator;
  GstCudaContext *ctx;

  if (mem->target != GST_CUDA_MEMORY_TARGET_DEVICE)
    return;

  g_mutex_lock (&mem->lock);
  mem->map_count--;
  GST_CAT_TRACE (GST_CAT_MEMORY,
      "unmap CUDA memory %p, map count %d, have map_data %s",
      mem, mem->map_count, mem->map_data ? "true" : "false");

  if (mem->map_count > 0 || !mem->map_data) {
    g_mutex_unlock (&mem->lock);
    return;
  }

  allocator = GST_CUDA_ALLOCATOR_CAST (GST_MEMORY_CAST (mem)->allocator);
  ctx = allocator->context;

  if (!gst_cuda_context_push (ctx)) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot push cuda context");
    /* Try below anyway */
  }

  if (mem->need_upload) {
    if (!gst_cuda_result (CuMemcpyHtoD ((CUdeviceptr) mem->data,
                mem->map_data, GST_MEMORY_CAST (mem)->maxsize))) {
      GST_CAT_ERROR (GST_CAT_MEMORY,
          "cannot copy host memory to device memory");
    }
  }

  if (!gst_cuda_result (CuMemFreeHost (mem->map_alloc_data))) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot free host memory");
  }

  if (!gst_cuda_context_pop ()) {
    GST_CAT_ERROR (GST_CAT_MEMORY, "cannot pop cuda context");
  }

  mem->map_alloc_data = mem->map_data = NULL;
  mem->need_upload = FALSE;
  g_mutex_unlock (&mem->lock);
}

static GstMemory *
cuda_mem_share (GstCudaMemory * mem, gssize offset, gssize size)
{
  return NULL;
}

static gboolean
cuda_mem_is_span (GstCudaMemory * mem1, GstCudaMemory * mem2, gsize * offset)
{
  return FALSE;
}


GstAllocator *
gst_cuda_allocator_new (GstCudaContext * context, GstCudaMemoryTarget target)
{
  GstCudaAllocator *allocator;

  g_return_val_if_fail (GST_IS_CUDA_CONTEXT (context), NULL);
  g_return_val_if_fail (target == GST_CUDA_MEMORY_TARGET_HOST ||
      target == GST_CUDA_MEMORY_TARGET_DEVICE, NULL);

  allocator = g_object_new (GST_TYPE_CUDA_ALLOCATOR, NULL);

  allocator->context = gst_object_ref (context);
  allocator->default_target = target;

  return GST_ALLOCATOR_CAST (allocator);
}

gboolean
gst_is_cuda_memory (GstMemory * mem)
{
  return mem != NULL && mem->allocator != NULL &&
      GST_IS_CUDA_ALLOCATOR (mem->allocator);
}

gboolean
gst_cuda_memory_get_target (GstCudaMemory * mem, GstCudaMemoryTarget * target)
{
  if (!gst_is_cuda_memory (GST_MEMORY_CAST (mem)))
    return FALSE;

  *target = mem->target;
  return TRUE;
}
