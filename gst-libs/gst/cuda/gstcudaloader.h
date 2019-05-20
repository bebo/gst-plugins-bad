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

#ifndef __GST_CUDA_LOADER_H__
#define __GST_CUDA_LOADER_H__

#include <gst/gst.h>
#include <gst/cuda/cuda-prelude.h>
#include <cuda.h>

#ifndef __GST_CUDA_PRIVATE_INSIDE_H__
#error "Only <gstcuda_private.h> can include this directly"
#endif

G_BEGIN_DECLS

GST_CUDA_API
gboolean gst_cuda_load_library (void);

/* cuda.h */
GST_CUDA_API
CUresult CuInit (unsigned int Flags);

GST_CUDA_API
CUresult CuGetErrorName (CUresult error, const char **pStr);

GST_CUDA_API
CUresult CuGetErrorString (CUresult error, const char **pStr);

GST_CUDA_API
CUresult CuCtxCreate (CUcontext *pctx, unsigned int flags, CUdevice dev);

GST_CUDA_API
CUresult CuCtxDestroy (CUcontext ctx);

GST_CUDA_API
CUresult CuCtxPopCurrent (CUcontext *pctx);

GST_CUDA_API
CUresult CuCtxPushCurrent (CUcontext ctx);

GST_CUDA_API
CUresult CuGraphicsMapResources (unsigned int count, CUgraphicsResource *resources, CUstream hStream);

GST_CUDA_API
CUresult CuGraphicsUnmapResources (unsigned int count, CUgraphicsResource *resources, CUstream hStream);

GST_CUDA_API
CUresult CuGraphicsSubResourceGetMappedArray (CUarray *pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);

GST_CUDA_API
CUresult CuGraphicsResourceGetMappedPointer (CUdeviceptr *pDevPtr, size_t *pSize, CUgraphicsResource resource);

GST_CUDA_API
CUresult CuGraphicsUnregisterResource (CUgraphicsResource resource);

GST_CUDA_API
CUresult CuMemAlloc (CUdeviceptr *dptr, unsigned int bytesize);

GST_CUDA_API
CUresult CuMemAllocPitch (CUdeviceptr *dptr, size_t *pPitch, size_t WidthInBytes, size_t Height, unsigned int ElementSizeBytes);

GST_CUDA_API
CUresult CuMemAllocHost (void **pp, unsigned int bytesize);

GST_CUDA_API
CUresult CuMemcpy2D (const CUDA_MEMCPY2D *pCopy);

GST_CUDA_API
CUresult CuMemcpy2DAsync (const CUDA_MEMCPY2D *pCopy, CUstream hStream);

GST_CUDA_API
CUresult CuMemcpyHtoD (CUdeviceptr dstDevice, const void *srcHost, unsigned int ByteCount);

GST_CUDA_API
CUresult CuMemcpyDtoH (void *dstHost, CUdeviceptr srcDevice, unsigned int ByteCount);

GST_CUDA_API
CUresult CuMemcpyDtoD (CUdeviceptr dstDevice, CUdeviceptr srcDevice, unsigned int ByteCount);

GST_CUDA_API
CUresult CuMemFree (CUdeviceptr dptr);

GST_CUDA_API
CUresult CuMemFreeHost (void *p);

GST_CUDA_API
CUresult CuStreamSynchronize (CUstream hStream);

GST_CUDA_API
CUresult CuDeviceGet (CUdevice *device, int ordinal);

GST_CUDA_API
CUresult CuDeviceGetCount (int *count);

GST_CUDA_API
CUresult CuDeviceGetName (char *name, int len, CUdevice dev);

GST_CUDA_API
CUresult CuDeviceGetAttribute (int *pi, CUdevice_attribute attrib, CUdevice dev);

/* cudaGL.h */
GST_CUDA_API
CUresult CuGraphicsGLRegisterImage (CUgraphicsResource *pCudaResource, unsigned int image, unsigned int target, unsigned int Flags);

GST_CUDA_API
CUresult CuGraphicsGLRegisterBuffer (CUgraphicsResource *pCudaResource, unsigned int buffer, unsigned int Flags);

G_END_DECLS

#endif /* __GST_CUDA_LOADER_H__ */
