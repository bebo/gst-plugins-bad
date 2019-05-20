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

#ifndef __GST_CUVID_LOADER_H__
#define __GST_CUVID_LOADER_H__

#include <gst/gst.h>
#include "nvcuvid.h"

G_BEGIN_DECLS

#if defined(__CUVID_DEVPTR64) && !defined(__CUVID_INTERNAL)
typedef unsigned long long cuvid_devptr_t;
#else
typedef unsigned int cuvid_devptr_t;
#endif

/* cuvid.h */
gboolean gst_cuvid_load_library (void);

CUresult CuvidCtxLockCreate (CUvideoctxlock *pLock, CUcontext ctx);
CUresult CuvidCtxLockDestroy (CUvideoctxlock lck);
CUresult CuvidCtxLock (CUvideoctxlock lck, unsigned int reserved_flags);
CUresult CuvidCtxUnlock (CUvideoctxlock lck, unsigned int reserved_flags);
CUresult CuvidCreateDecoder (CUvideodecoder *phDecoder, CUVIDDECODECREATEINFO *pdci);
CUresult CuvidDestroyDecoder (CUvideodecoder hDecoder);
CUresult CuvidDecodePicture (CUvideodecoder hDecoder, CUVIDPICPARAMS *pPicParams);
CUresult CuvidCreateVideoParser (CUvideoparser *pObj, CUVIDPARSERPARAMS *pParams);
CUresult CuvidParseVideoData (CUvideoparser obj, CUVIDSOURCEDATAPACKET *pPacket);
CUresult CuvidDestroyVideoParser (CUvideoparser obj);
CUresult CuvidMapVideoFrame (CUvideodecoder hDecoder, int nPicIdx, cuvid_devptr_t *pDevPtr, unsigned int *pPitch, CUVIDPROCPARAMS *pVPP);
CUresult CuvidUnmapVideoFrame(CUvideodecoder hDecoder, cuvid_devptr_t DevPtr);

G_END_DECLS

#endif /* __GST_CUVID_LOADER_H__ */