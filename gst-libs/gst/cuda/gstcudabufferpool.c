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

#include "gstcudabufferpool.h"
#include "gstcudacontext.h"
#include "gstcuda_private.h"

GST_DEBUG_CATEGORY_STATIC (gst_cuda_buffer_pool_debug);
#define GST_CAT_DEFAULT gst_cuda_buffer_pool_debug

struct _GstCudaBufferPoolPrivate
{
  GstCudaContext *context;
  GstAllocator *allocator;
  GstVideoInfo info;
  gboolean add_videometa;
  GstCudaMemoryTarget target;
};

#define gst_cuda_buffer_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstCudaBufferPool, gst_cuda_buffer_pool,
    GST_TYPE_BUFFER_POOL);

static const gchar **
gst_cuda_buffer_pool_get_options (GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META,
    GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT, NULL
  };

  return options;
}

static gboolean
gst_cuda_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstCudaBufferPool *cuda_pool = GST_CUDA_BUFFER_POOL_CAST (pool);
  GstCudaBufferPoolPrivate *priv = cuda_pool->priv;
  GstCaps *caps = NULL;
  guint size, min_buffers, max_buffers;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;
  GstVideoInfo *info;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, &params))
    goto wrong_config;

  /* now parse the caps from the config */
  if (!gst_video_info_from_caps (&priv->info, caps))
    goto wrong_caps;

  info = &priv->info;

  GST_LOG_OBJECT (pool, "%dx%d, caps %" GST_PTR_FORMAT,
      GST_VIDEO_INFO_WIDTH (info), GST_VIDEO_INFO_HEIGHT (info), caps);

  gst_clear_object (&priv->allocator);

  if (allocator) {
    if (!GST_IS_CUDA_ALLOCATOR (allocator)) {
      goto wrong_allocator;
    } else {
      priv->allocator = gst_object_ref (allocator);
    }
  } else {
    priv->allocator = gst_cuda_allocator_new (priv->context, priv->target);
    if (G_UNLIKELY (priv->allocator == NULL))
      goto no_allocator;
  }

  priv->add_videometa = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  /* FIXME: add align, videometa */

  gst_buffer_pool_config_set_params (config, caps, GST_VIDEO_INFO_SIZE (info),
      min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);

  /* ERRORS */
wrong_config:
  {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_caps:
  {
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
no_allocator:
  {
    GST_WARNING_OBJECT (pool, "Could not create new CUDA allocator");
    return FALSE;
  }
wrong_allocator:
  {
    GST_WARNING_OBJECT (pool, "Incorrect allocator type for this pool");
    return FALSE;
  }
}

static GstFlowReturn
gst_cuda_buffer_pool_alloc (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  GstCudaBufferPool *cuda_pool = GST_CUDA_BUFFER_POOL_CAST (pool);
  GstCudaBufferPoolPrivate *priv = cuda_pool->priv;
  GstVideoInfo *info;
  GstBuffer *cuda;
  GstMemory *mem;

  info = &priv->info;

  cuda = gst_buffer_new ();

  mem = gst_allocator_alloc (GST_ALLOCATOR_CAST (priv->allocator),
      GST_VIDEO_INFO_SIZE (info), NULL);

  if (mem == NULL) {
    gst_buffer_unref (cuda);
    GST_WARNING_OBJECT (pool, "Cannot create CUDA memory");
    return GST_FLOW_ERROR;
  }
  gst_buffer_append_memory (cuda, mem);

  if (priv->add_videometa) {
    GST_DEBUG_OBJECT (pool, "adding GstVideoMeta");
    gst_buffer_add_video_meta_full (cuda, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_INFO_FORMAT (info), GST_VIDEO_INFO_WIDTH (info),
        GST_VIDEO_INFO_HEIGHT (info), GST_VIDEO_INFO_N_PLANES (info),
        info->offset, info->stride);
  }

  *buffer = cuda;

  return GST_FLOW_OK;
}

GstBufferPool *
gst_cuda_buffer_pool_new (GstCudaContext * context, GstCudaMemoryTarget target)
{
  GstCudaBufferPool *pool;

  pool = g_object_new (GST_TYPE_CUDA_BUFFER_POOL, NULL);
  gst_object_ref_sink (pool);

  pool->priv->context = gst_object_ref (context);
  pool->priv->target = target;

  GST_LOG_OBJECT (pool, "new CUDA buffer pool %p", pool);

  return GST_BUFFER_POOL_CAST (pool);
}

static void
gst_cuda_buffer_pool_dispose (GObject * object)
{
  GstCudaBufferPool *pool = GST_CUDA_BUFFER_POOL_CAST (object);
  GstCudaBufferPoolPrivate *priv = pool->priv;

  GST_LOG_OBJECT (pool, "finalize CUDA buffer pool %p", pool);

  gst_clear_object (&priv->allocator);
  gst_clear_object (&priv->context);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}


static void
gst_cuda_buffer_pool_class_init (GstCudaBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->dispose = gst_cuda_buffer_pool_dispose;

  gstbufferpool_class->get_options = gst_cuda_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_cuda_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_cuda_buffer_pool_alloc;

  GST_DEBUG_CATEGORY_INIT (gst_cuda_buffer_pool_debug, "cudabufferpool", 0,
      "CUDA Buffer Pool");
}

static void
gst_cuda_buffer_pool_init (GstCudaBufferPool * pool)
{
  pool->priv = gst_cuda_buffer_pool_get_instance_private (pool);
}
