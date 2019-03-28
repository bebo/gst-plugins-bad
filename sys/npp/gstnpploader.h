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

#ifndef __GST_NPP_LOADER_H__
#define __GST_NPP_LOADER_H__

#include <gst/gst.h>
#include <gmodule.h>
#include <npp.h>

G_BEGIN_DECLS

gboolean gst_cuda_load_npp_library (void);

/* nppi_geometry_transforms.h*/
NppStatus NppiResizeSqrPixel_8u_C1R (const Npp8u * pSrc,
                                     NppiSize oSrcSize,
                                     int nSrcStep,
                                     NppiRect oSrcROI,
                                     Npp8u * pDst,
                                     int nDstStep,
                                     NppiRect oDstROI,
                                     double nXFactor,
                                     double nYFactor,
                                     double nXShift,
                                     double nYShift,
                                     int eInterpolation);

/* nppi_color_conversion.h */
NppStatus NppiYCbCr420_8u_P3P2R (const Npp8u * const pSrc[3],
                                 int rSrcStep[3],
                                 Npp8u * pDstY,
                                 int nDstYStep,
                                 Npp8u * pDstCbCr,
                                 int nDstCbCrStep,
                                 NppiSize oSizeROI);

NppStatus NppiYCbCr420_8u_P2P3R (const Npp8u * const pSrcY,
                                 int nSrcYStep,
                                 const Npp8u * pSrcCbCr,
                                 int nSrcCbCrStep,
                                 Npp8u * pDst[3],
                                 int rDstStep[3],
                                 NppiSize oSizeROI);

G_END_DECLS

#endif /* __GST_NPP_LOADER_H__ */