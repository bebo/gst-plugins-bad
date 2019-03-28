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

#include "gstnpploader.h"
#include <gmodule.h>

#ifndef G_OS_WIN32
#define NPPIG_LIBNAME "libnppig.so"
#endif

#ifndef G_OS_WIN32
#define NPPICC_LIBNAME "libnppicc.so"
#endif

#define LOAD_SYMBOL(name,func) G_STMT_START { \
  if (!g_module_symbol (module, G_STRINGIFY (name), (gpointer *) &vtable->func)) { \
    GST_ERROR ("Failed to load '%s' from %s, %s", G_STRINGIFY (name), filename, g_module_error()); \
    goto error; \
  } \
} G_STMT_END;

/* *INDENT-OFF* */
typedef struct _GstNvCodecNppIgVTable
{
  GModule *module;
  NppStatus (*NppiResizeSqrPixel_8u_C1R) (const Npp8u * pSrc, NppiSize oSrcSize, int nSrcStep, NppiRect oSrcROI,
                                                Npp8u * pDst, int nDstStep, NppiRect oDstROI,
                                          double nXFactor, double nYFactor, double nXShift, double nYShift, int eInterpolation);
} GstNvCodecNppIgVTable;

typedef struct _GstNvCodecNppIccVTable
{
  GModule *module;
  NppStatus (*NppiYCbCr420_8u_P3P2R) (const Npp8u * const pSrc[3],
                                      int rSrcStep[3],
                                      Npp8u * pDstY,
                                      int nDstYStep,
                                      Npp8u * pDstCbCr,
                                      int nDstCbCrStep,
                                      NppiSize oSizeROI);

  NppStatus (*NppiYCbCr420_8u_P2P3R) (const Npp8u * const pSrcY,
                                      int nSrcYStep,
                                      const Npp8u * pSrcCbCr,
                                      int nSrcCbCrStep,
                                      Npp8u * pDst[3],
                                      int rDstStep[3],
                                      NppiSize oSizeROI);
} GstNvCodecNppIccVTable;
/* *INDENT-ON* */

static GstNvCodecNppIgVTable *gst_nppig_vtable = NULL;
static GstNvCodecNppIccVTable *gst_nppicc_vtable = NULL;

#ifndef G_OS_WIN32
static gboolean
gst_npp_load_nppig (void)
{
  GModule *module;
  const gchar *filename = NPPIG_LIBNAME;
  GstNvCodecNppIgVTable *vtable;

  if (gst_nppig_vtable)
    return TRUE;

  module = g_module_open (filename, G_MODULE_BIND_LAZY);
  if (module == NULL) {
    GST_ERROR ("Could not open library %s, %s", filename, g_module_error ());
    return FALSE;
  }

  vtable = g_slice_new0 (GstNvCodecNppIgVTable);

  LOAD_SYMBOL (nppiResizeSqrPixel_8u_C1R, NppiResizeSqrPixel_8u_C1R);

  vtable->module = module;
  gst_nppig_vtable = vtable;


  return TRUE;

error:
  g_module_close (module);
  g_slice_free (GstNvCodecNppIgVTable, vtable);

  return FALSE;
}

static gboolean
gst_npp_load_nppicc (void)
{
  GModule *module;
  const gchar *filename = NPPICC_LIBNAME;
  GstNvCodecNppIccVTable *vtable;

  if (gst_nppicc_vtable)
    return TRUE;

  module = g_module_open (filename, G_MODULE_BIND_LAZY);
  if (module == NULL) {
    GST_ERROR ("Could not open library %s, %s", filename, g_module_error ());
    return FALSE;
  }

  vtable = g_slice_new0 (GstNvCodecNppIccVTable);

  LOAD_SYMBOL (nppiYCbCr420_8u_P3P2R, NppiYCbCr420_8u_P3P2R);
  LOAD_SYMBOL (nppiYCbCr420_8u_P2P3R, NppiYCbCr420_8u_P2P3R);

  vtable->module = module;
  gst_nppicc_vtable = vtable;

  return TRUE;

error:
  g_module_close (module);
  g_slice_free (GstNvCodecNppIccVTable, vtable);

  return FALSE;
}

#else /* G_OS_WIN32 */
/* NOTE: on Windows, npp library will not installed in system library path, so
 * g_module_open() will not work in most cases */
static gboolean
gst_npp_load_nppig (void)
{
  GstNvCodecNppIgVTable *vtable;

  if (gst_nppig_vtable)
    return TRUE;

  vtable = g_slice_new0 (GstNvCodecNppIgVTable);

  vtable->NppiResizeSqrPixel_8u_C1R = nppiResizeSqrPixel_8u_C1R;
  gst_nppig_vtable = vtable;

  return TRUE;
}

static gboolean
gst_npp_load_nppicc (void)
{
  GstNvCodecNppIccVTable *vtable;

  if (gst_nppicc_vtable)
    return TRUE;

  vtable = g_slice_new0 (GstNvCodecNppIccVTable);

  vtable->NppiYCbCr420_8u_P3P2R = nppiYCbCr420_8u_P3P2R;
  vtable->NppiYCbCr420_8u_P2P3R = nppiYCbCr420_8u_P2P3R;
  gst_nppicc_vtable = vtable;

  return TRUE;
}
#endif /* G_OS_WIN32 */

gboolean
gst_cuda_load_npp_library (void)
{
  gboolean ret = TRUE;

  ret &= gst_npp_load_nppig ();
  ret &= gst_npp_load_nppicc ();

  return ret;
}

NppStatus
NppiResizeSqrPixel_8u_C1R (const Npp8u * pSrc, NppiSize oSrcSize, int nSrcStep,
    NppiRect oSrcROI, Npp8u * pDst, int nDstStep, NppiRect oDstROI,
    double nXFactor, double nYFactor, double nXShift, double nYShift,
    int eInterpolation)
{
  g_assert (gst_nppig_vtable != NULL);
  g_assert (gst_nppig_vtable->NppiResizeSqrPixel_8u_C1R != NULL);

  return gst_nppig_vtable->NppiResizeSqrPixel_8u_C1R (pSrc, oSrcSize, nSrcStep,
      oSrcROI, pDst, nDstStep, oDstROI, nXFactor, nYFactor, nXShift, nYShift,
      eInterpolation);
}

NppStatus
NppiYCbCr420_8u_P3P2R (const Npp8u * const pSrc[3], int rSrcStep[3],
    Npp8u * pDstY, int nDstYStep, Npp8u * pDstCbCr, int nDstCbCrStep,
    NppiSize oSizeROI)
{
  g_assert (gst_nppicc_vtable != NULL);
  g_assert (gst_nppicc_vtable->NppiYCbCr420_8u_P3P2R != NULL);

  return gst_nppicc_vtable->NppiYCbCr420_8u_P3P2R (pSrc, rSrcStep, pDstY,
      nDstYStep, pDstCbCr, nDstCbCrStep, oSizeROI);
}

NppStatus
NppiYCbCr420_8u_P2P3R (const Npp8u * const pSrcY, int nSrcYStep,
    const Npp8u * pSrcCbCr, int nSrcCbCrStep, Npp8u * pDst[3], int rDstStep[3],
    NppiSize oSizeROI)
{
  g_assert (gst_nppicc_vtable != NULL);
  g_assert (gst_nppicc_vtable->NppiYCbCr420_8u_P2P3R != NULL);

  return gst_nppicc_vtable->NppiYCbCr420_8u_P2P3R (pSrcY, nSrcYStep,
      pSrcCbCr, nSrcCbCrStep, pDst, rDstStep, oSizeROI);
}
