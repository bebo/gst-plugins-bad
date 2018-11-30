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
#include <cuda.h>

G_BEGIN_DECLS

gboolean gst_cuda_load_library (void);

/* cuda.h */
CUresult CuInit (unsigned int Flags);
CUresult CuGetErrorName (CUresult error, const char **pStr);
CUresult CuGetErrorString (CUresult error, const char **pStr);
CUresult CuCtxCreate (CUcontext *pctx, unsigned int flags, CUdevice dev);
CUresult CuCtxDestroy (CUcontext ctx);
CUresult CuCtxPopCurrent (CUcontext *pctx);
CUresult CuCtxPushCurrent (CUcontext ctx);

CUresult CuGraphicsMapResources (unsigned int count, CUgraphicsResource *resources, CUstream hStream);
CUresult CuGraphicsUnmapResources (unsigned int count, CUgraphicsResource *resources, CUstream hStream);
CUresult CuGraphicsSubResourceGetMappedArray (CUarray *pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);
CUresult CuGraphicsResourceGetMappedPointer (CUdeviceptr *pDevPtr, size_t *pSize, CUgraphicsResource resource);
CUresult CuGraphicsUnregisterResource (CUgraphicsResource resource);

CUresult CuMemAlloc (CUdeviceptr *dptr, unsigned int bytesize);
CUresult CuMemAllocPitch (CUdeviceptr *dptr, size_t *pPitch, size_t WidthInBytes, size_t Height, unsigned int ElementSizeBytes);
CUresult CuMemcpy2D (const CUDA_MEMCPY2D *pCopy);
CUresult CuMemcpy2DAsync (const CUDA_MEMCPY2D *pCopy, CUstream hStream);
CUresult CuMemFree (CUdeviceptr dptr);

CUresult CuStreamSynchronize (CUstream hStream);

CUresult CuDeviceGet (CUdevice *device, int ordinal);
CUresult CuDeviceGetCount (int *count);
CUresult CuDeviceGetName (char *name, int len, CUdevice dev);
CUresult CuDeviceGetAttribute (int *pi, CUdevice_attribute attrib, CUdevice dev);

/* cudaGL.h */
CUresult CuGraphicsGLRegisterImage (CUgraphicsResource *pCudaResource, unsigned int image, unsigned int target, unsigned int Flags);
CUresult CuGraphicsGLRegisterBuffer (CUgraphicsResource *pCudaResource, unsigned int buffer, unsigned int Flags);

G_END_DECLS

#endif /* __GST_CUDA_LOADER_H__ */
