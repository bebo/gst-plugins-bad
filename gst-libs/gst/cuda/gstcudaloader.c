/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
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

#include "gstcuda_private.h"
#include <gmodule.h>

G_LOCK_DEFINE_STATIC (init_lock);

#ifndef G_OS_WIN32
#define CUDA_LIBNAME "libcuda.so.1"
#else
#define CUDA_LIBNAME "nvcuda.dll"
#endif

#define LOAD_SYMBOL(name,func) G_STMT_START { \
  if (!g_module_symbol (module, G_STRINGIFY (name), (gpointer *) &vtable->func)) { \
    GST_ERROR ("Failed to load '%s' from %s, %s", G_STRINGIFY (name), filename, g_module_error()); \
    goto error; \
  } \
} G_STMT_END;

/* *INDENT-OFF* */
typedef struct _GstCudaVTable
{
  GModule *module;
  CUresult (*CuInit) (unsigned int Flags);
  CUresult (*CuGetErrorName) (CUresult error, const char **pStr);
  CUresult (*CuGetErrorString) (CUresult error, const char **pStr);

  CUresult (*CuCtxCreate) (CUcontext * pctx, unsigned int flags, CUdevice dev);
  CUresult (*CuCtxDestroy) (CUcontext ctx);
  CUresult (*CuCtxPopCurrent) (CUcontext * pctx);
  CUresult (*CuCtxPushCurrent) (CUcontext ctx);

  CUresult (*CuGraphicsMapResources) (unsigned int count, CUgraphicsResource * resources, CUstream hStream);
  CUresult (*CuGraphicsUnmapResources) (unsigned int count, CUgraphicsResource * resources, CUstream hStream);
  CUresult (*CuGraphicsSubResourceGetMappedArray) (CUarray * pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);
  CUresult (*CuGraphicsResourceGetMappedPointer) (CUdeviceptr * pDevPtr, size_t * pSize, CUgraphicsResource resource);
  CUresult (*CuGraphicsUnregisterResource) (CUgraphicsResource resource);

  CUresult (*CuMemAlloc) (CUdeviceptr * dptr, unsigned int bytesize);
  CUresult (*CuMemAllocPitch) (CUdeviceptr * dptr, size_t * pPitch, size_t WidthInBytes, size_t Height, unsigned int ElementSizeBytes);
  CUresult (*CuMemAllocHost) (void **pp, unsigned int bytesize);
  CUresult (*CuMemcpy2D) (const CUDA_MEMCPY2D * pCopy);
  CUresult (*CuMemcpy2DAsync) (const CUDA_MEMCPY2D *pCopy, CUstream hStream);
  CUresult (*CuMemcpyHtoD) (CUdeviceptr dstDevice, const void *srcHost, unsigned int ByteCount);
  CUresult (*CuMemcpyDtoH) (void *dstHost, CUdeviceptr srcDevice, unsigned int ByteCount);
  CUresult (*CuMemcpyDtoD) (CUdeviceptr dstDevice, CUdeviceptr srcDevice, unsigned int ByteCount);
  CUresult (*CuMemFree) (CUdeviceptr dptr);
  CUresult (*CuMemFreeHost) (void *p);

  CUresult (*CuStreamSynchronize) (CUstream hStream);

  CUresult (*CuDeviceGet) (CUdevice * device, int ordinal);
  CUresult (*CuDeviceGetCount) (int *count);
  CUresult (*CuDeviceGetName) (char *name, int len, CUdevice dev);
  CUresult (*CuDeviceGetAttribute) (int *pi, CUdevice_attribute attrib, CUdevice dev);

  CUresult (*CuGraphicsGLRegisterImage) (CUgraphicsResource * pCudaResource, unsigned int image, unsigned int target, unsigned int Flags);
  CUresult (*CuGraphicsGLRegisterBuffer) (CUgraphicsResource * pCudaResource, unsigned int buffer, unsigned int Flags);
} GstCudaVTable;
/* *INDENT-ON* */

static GstCudaVTable *gst_cuda_vtable = NULL;

gboolean
gst_cuda_load_library (void)
{
  GModule *module;
  const gchar *filename = CUDA_LIBNAME;
  GstCudaVTable *vtable;

  G_LOCK (init_lock);
  if (gst_cuda_vtable) {
    G_UNLOCK (init_lock);
    return TRUE;
  }

  module = g_module_open (filename, G_MODULE_BIND_LAZY);
  if (module == NULL) {
    G_UNLOCK (init_lock);
    GST_ERROR ("Could not open library %s, %s", filename, g_module_error ());
    return FALSE;
  }

  vtable = g_slice_new0 (GstCudaVTable);

  /* cuda.h */
  LOAD_SYMBOL (cuInit, CuInit);
  LOAD_SYMBOL (cuGetErrorName, CuGetErrorName);
  LOAD_SYMBOL (cuGetErrorString, CuGetErrorString);
  LOAD_SYMBOL (cuCtxCreate, CuCtxCreate);
  LOAD_SYMBOL (cuCtxDestroy, CuCtxDestroy);
  LOAD_SYMBOL (cuCtxPopCurrent, CuCtxPopCurrent);
  LOAD_SYMBOL (cuCtxPushCurrent, CuCtxPushCurrent);

  LOAD_SYMBOL (cuGraphicsMapResources, CuGraphicsMapResources);
  LOAD_SYMBOL (cuGraphicsUnmapResources, CuGraphicsUnmapResources);
  LOAD_SYMBOL (cuGraphicsSubResourceGetMappedArray,
      CuGraphicsSubResourceGetMappedArray);
  LOAD_SYMBOL (cuGraphicsResourceGetMappedPointer,
      CuGraphicsResourceGetMappedPointer);
  LOAD_SYMBOL (cuGraphicsUnregisterResource, CuGraphicsUnregisterResource);

  LOAD_SYMBOL (cuMemAlloc, CuMemAlloc);
  LOAD_SYMBOL (cuMemAllocPitch, CuMemAllocPitch);
  LOAD_SYMBOL (cuMemAllocHost, CuMemAllocHost);
  LOAD_SYMBOL (cuMemcpy2D, CuMemcpy2D);
  LOAD_SYMBOL (cuMemcpy2DAsync, CuMemcpy2DAsync);
  LOAD_SYMBOL (cuMemcpyHtoD, CuMemcpyHtoD);
  LOAD_SYMBOL (cuMemcpyDtoH, CuMemcpyDtoH);
  LOAD_SYMBOL (cuMemcpyDtoD, CuMemcpyDtoD);
  LOAD_SYMBOL (cuMemFree, CuMemFree);
  LOAD_SYMBOL (cuMemFreeHost, CuMemFreeHost);

  LOAD_SYMBOL (cuStreamSynchronize, CuStreamSynchronize);

  LOAD_SYMBOL (cuDeviceGet, CuDeviceGet);
  LOAD_SYMBOL (cuDeviceGetCount, CuDeviceGetCount);
  LOAD_SYMBOL (cuDeviceGetName, CuDeviceGetName);
  LOAD_SYMBOL (cuDeviceGetAttribute, CuDeviceGetAttribute);

  /* cudaGL.h */
  LOAD_SYMBOL (cuGraphicsGLRegisterImage, CuGraphicsGLRegisterImage);
  LOAD_SYMBOL (cuGraphicsGLRegisterBuffer, CuGraphicsGLRegisterBuffer);

  vtable->module = module;
  gst_cuda_vtable = vtable;
  G_UNLOCK (init_lock);

  return TRUE;

error:
  g_module_close (module);
  g_slice_free (GstCudaVTable, vtable);
  G_UNLOCK (init_lock);

  return FALSE;
}

CUresult
CuInit (unsigned int Flags)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuInit != NULL);

  return gst_cuda_vtable->CuInit (Flags);
}

CUresult
CuGetErrorName (CUresult error, const char **pStr)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGetErrorName != NULL);

  return gst_cuda_vtable->CuGetErrorName (error, pStr);
}

CUresult
CuGetErrorString (CUresult error, const char **pStr)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGetErrorString != NULL);

  return gst_cuda_vtable->CuGetErrorString (error, pStr);
}

CUresult
CuCtxCreate (CUcontext * pctx, unsigned int flags, CUdevice dev)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuCtxCreate != NULL);

  return gst_cuda_vtable->CuCtxCreate (pctx, flags, dev);
}

CUresult
CuCtxDestroy (CUcontext ctx)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuCtxDestroy != NULL);

  return gst_cuda_vtable->CuCtxDestroy (ctx);
}

CUresult
CuCtxPopCurrent (CUcontext * pctx)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuCtxPopCurrent != NULL);

  return gst_cuda_vtable->CuCtxPopCurrent (pctx);
}

CUresult
CuCtxPushCurrent (CUcontext ctx)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuCtxPushCurrent != NULL);

  return gst_cuda_vtable->CuCtxPushCurrent (ctx);
}

CUresult
CuGraphicsMapResources (unsigned int count, CUgraphicsResource * resources,
    CUstream hStream)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsMapResources != NULL);

  return gst_cuda_vtable->CuGraphicsMapResources (count, resources, hStream);
}

CUresult
CuGraphicsUnmapResources (unsigned int count, CUgraphicsResource * resources,
    CUstream hStream)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsUnmapResources != NULL);

  return gst_cuda_vtable->CuGraphicsUnmapResources (count, resources, hStream);
}

CUresult
CuGraphicsSubResourceGetMappedArray (CUarray * pArray,
    CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsSubResourceGetMappedArray != NULL);

  return gst_cuda_vtable->CuGraphicsSubResourceGetMappedArray (pArray, resource,
      arrayIndex, mipLevel);
}

CUresult
CuGraphicsResourceGetMappedPointer (CUdeviceptr * pDevPtr, size_t * pSize,
    CUgraphicsResource resource)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsResourceGetMappedPointer != NULL);

  return gst_cuda_vtable->CuGraphicsResourceGetMappedPointer (pDevPtr, pSize,
      resource);
}

CUresult
CuGraphicsUnregisterResource (CUgraphicsResource resource)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsUnregisterResource != NULL);

  return gst_cuda_vtable->CuGraphicsUnregisterResource (resource);
}

CUresult
CuMemAlloc (CUdeviceptr * dptr, unsigned int bytesize)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemAlloc != NULL);

  return gst_cuda_vtable->CuMemAlloc (dptr, bytesize);
}

CUresult
CuMemAllocPitch (CUdeviceptr * dptr, size_t * pPitch, size_t WidthInBytes,
    size_t Height, unsigned int ElementSizeBytes)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemAllocPitch != NULL);

  return gst_cuda_vtable->CuMemAllocPitch (dptr, pPitch, WidthInBytes, Height,
      ElementSizeBytes);
}

CUresult
CuMemAllocHost (void **pp, unsigned int bytesize)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemAllocHost != NULL);

  return gst_cuda_vtable->CuMemAllocHost (pp, bytesize);
}

CUresult
CuMemcpy2D (const CUDA_MEMCPY2D * pCopy)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemcpy2D != NULL);

  return gst_cuda_vtable->CuMemcpy2D (pCopy);
}

CUresult
CuMemcpy2DAsync (const CUDA_MEMCPY2D * pCopy, CUstream hStream)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemcpy2DAsync != NULL);

  return gst_cuda_vtable->CuMemcpy2DAsync (pCopy, hStream);
}

CUresult
CuMemcpyHtoD (CUdeviceptr dstDevice, const void *srcHost,
    unsigned int ByteCount)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemcpyHtoD != NULL);

  return gst_cuda_vtable->CuMemcpyHtoD (dstDevice, srcHost, ByteCount);
}

CUresult
CuMemcpyDtoH (void *dstHost, CUdeviceptr srcDevice, unsigned int ByteCount)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemcpyDtoH != NULL);

  return gst_cuda_vtable->CuMemcpyDtoH (dstHost, srcDevice, ByteCount);
}

CUresult
CuMemcpyDtoD (CUdeviceptr dstDevice, CUdeviceptr srcDevice,
    unsigned int ByteCount)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemcpyDtoD != NULL);

  return gst_cuda_vtable->CuMemcpyDtoD (dstDevice, srcDevice, ByteCount);
}

CUresult
CuMemFree (CUdeviceptr dptr)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemFree != NULL);

  return gst_cuda_vtable->CuMemFree (dptr);
}

CUresult
CuMemFreeHost (void *p)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuMemFreeHost != NULL);

  return gst_cuda_vtable->CuMemFreeHost (p);
}

CUresult
CuStreamSynchronize (CUstream hStream)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuStreamSynchronize != NULL);

  return gst_cuda_vtable->CuStreamSynchronize (hStream);
}

CUresult
CuDeviceGet (CUdevice * device, int ordinal)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuDeviceGet != NULL);

  return gst_cuda_vtable->CuDeviceGet (device, ordinal);
}

CUresult
CuDeviceGetCount (int *count)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuDeviceGetCount != NULL);

  return gst_cuda_vtable->CuDeviceGetCount (count);
}

CUresult
CuDeviceGetName (char *name, int len, CUdevice dev)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuDeviceGetName != NULL);

  return gst_cuda_vtable->CuDeviceGetName (name, len, dev);
}

CUresult
CuDeviceGetAttribute (int *pi, CUdevice_attribute attrib, CUdevice dev)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuDeviceGetAttribute != NULL);

  return gst_cuda_vtable->CuDeviceGetAttribute (pi, attrib, dev);
}

/* cudaGL.h */
CUresult
CuGraphicsGLRegisterImage (CUgraphicsResource * pCudaResource,
    unsigned int image, unsigned int target, unsigned int Flags)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsGLRegisterImage != NULL);

  return gst_cuda_vtable->CuGraphicsGLRegisterImage (pCudaResource, image,
      target, Flags);
}

CUresult
CuGraphicsGLRegisterBuffer (CUgraphicsResource * pCudaResource,
    unsigned int buffer, unsigned int Flags)
{
  g_assert (gst_cuda_vtable != NULL);
  g_assert (gst_cuda_vtable->CuGraphicsGLRegisterBuffer != NULL);

  return gst_cuda_vtable->CuGraphicsGLRegisterBuffer (pCudaResource, buffer,
      Flags);
}