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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstnppscale.h"

GST_DEBUG_CATEGORY_STATIC (gst_npp_scale_debug);
#define GST_CAT_DEFAULT gst_npp_scale_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

#define SUPPORTED_FORMAT \
  "{ NV12 }"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, SUPPORTED_FORMAT))
    );

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, SUPPORTED_FORMAT))
    );

enum
{
  PROP_0,
  PROP_SCALE_MODE,
};

#define PROP_DEVICE_ID_DEFAULT    0
#define PROP_SCALE_MODE_DEFAULT   3

enum
{
  GST_NPP_SCALE_UNKNOWN = 0,
  GST_NPP_SCALE_NN = 1,
  GST_NPP_SCALE_LINEAR,
  GST_NPP_SCALE_CUBIC,
  GST_NPP_SCALE_CUBIC2P_BSPLINE,
  GST_NPP_SCALE_CUBIC2P_CATMULLROM,
  GST_NPP_SCALE_CUBIC2P_B05C03,
  GST_NPP_SCALE_SUPER,
  GST_NPP_SCALE_LANCZOS,
  GST_NPP_SCALE_LANCZOS3_ADVANCED
};

enum NppScaleStage
{
  NPP_STAGE_DEINTERLEAVE = 0,
  NPP_STAGE_RESIZE,
  NPP_STAGE_INTERLEAVE,
};

#define GST_NPP_SCALE_MODE_TYPE (gst_npp_scale_scale_mode_get_type())
static GType
gst_npp_scale_scale_mode_get_type (void)
{
  static GType scale_mode = 0;

  static const GEnumValue scale_modes[] = {
    {GST_NPP_SCALE_NN, "Nearest neighbor filtering", "nearest-neighbour"},
    {GST_NPP_SCALE_LINEAR, "Linear interpolation", "linear"},
    {GST_NPP_SCALE_CUBIC, "Cubic interpolation", "cubic"},
    {GST_NPP_SCALE_CUBIC2P_BSPLINE,
        "Two-parameter cubic filter (B=1, C=0)", "cubic2p-bspline"},
    {GST_NPP_SCALE_CUBIC2P_CATMULLROM,
        "Two-parameter cubic filter (B=0, C=1/2)", "cubic2p-catmullrom"},
    {GST_NPP_SCALE_CUBIC2P_B05C03,
        "Two-parameter cubic filter (B=1/2, C=3/10)", "cubic2p-b05c03"},
    {GST_NPP_SCALE_SUPER, "Super sampling", "super"},
    {GST_NPP_SCALE_LANCZOS, "Lanczos filtering", "lanczos"},
    {GST_NPP_SCALE_LANCZOS3_ADVANCED,
        "Generic Lanczos filtering with order 3", "lanczos-advanced"},
    {0, NULL, NULL}
  };

  if (!scale_mode) {
    scale_mode = g_enum_register_static ("GstNppScaleMode", scale_modes);
  }
  return scale_mode;
}

#define gst_npp_scale_parent_class parent_class
G_DEFINE_TYPE (GstNppScale, gst_npp_scale, GST_TYPE_CUDA_BASE_FILTER);

static void gst_npp_scale_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_npp_scale_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_npp_scale_dispose (GObject * object);
static GstCaps *gst_npp_scale_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_npp_scale_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static GstFlowReturn gst_npp_scale_transform_frame (GstCudaBaseFilter * filter,
    GstCudaMemoryTarget in_target, GstVideoFrame * in_frame,
    GstCudaMemoryTarget out_target, GstVideoFrame * out_frame);
static gboolean gst_npp_scale_set_info (GstCudaBaseFilter * filter,
    GstCaps * incaps, GstVideoInfo * in_info, GstCaps * outcaps,
    GstVideoInfo * out_info);

static void
gst_npp_scale_class_init (GstNppScaleClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseTransformClass *trans_class;
  GstCudaBaseFilterClass *bfilter_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  bfilter_class = GST_CUDA_BASE_FILTER_CLASS (klass);

  gobject_class->set_property = gst_npp_scale_set_property;
  gobject_class->get_property = gst_npp_scale_get_property;
  gobject_class->dispose = gst_npp_scale_dispose;

  g_object_class_install_property (gobject_class, PROP_SCALE_MODE,
      g_param_spec_enum ("scale-mode", "Scale Mode",
          "Rescale algorithm to use", GST_NPP_SCALE_MODE_TYPE,
          PROP_SCALE_MODE_DEFAULT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  gst_element_class_set_static_metadata (element_class,
      "NPP scale",
      "Filter/Converter/Video/Scaler",
      "NVIDIA GPU-accelerated video signal processing",
      "Seungha Yang <seungha.yang@navercorp.com>");

  trans_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_npp_scale_transform_caps);
  trans_class->fixate_caps = GST_DEBUG_FUNCPTR (gst_npp_scale_fixate_caps);

  bfilter_class->set_info = GST_DEBUG_FUNCPTR (gst_npp_scale_set_info);
  bfilter_class->transform_frame =
      GST_DEBUG_FUNCPTR (gst_npp_scale_transform_frame);

  GST_DEBUG_CATEGORY_INIT (gst_npp_scale_debug,
      "nppscale", 0, "Video Convert via NVIDIA Performance Primitives");
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
}

static void
gst_npp_scale_init (GstNppScale * npp)
{
  npp->scale_mode = PROP_SCALE_MODE_DEFAULT;
}

static void
gst_npp_scale_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNppScale *npp = GST_NPP_SCALE (object);

  switch (prop_id) {
    case PROP_SCALE_MODE:
      npp->scale_mode = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_npp_scale_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstNppScale *npp = GST_NPP_SCALE (object);

  switch (prop_id) {
    case PROP_SCALE_MODE:
      g_value_set_enum (value, npp->scale_mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_npp_scale_dispose (GObject * object)
{
  GstNppScale *npp = GST_NPP_SCALE (object);
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (object);
  gint i;

  if (!filter->context || !gst_cuda_context_push (filter->context))
    goto done;

  for (i = 0; i < G_N_ELEMENTS (npp->stage); i++) {
    if (npp->stage[i].out_buf)
      CuMemFree (npp->stage[i].out_buf);
    npp->stage[i].out_buf = 0;
  }

  if (npp->in_fallback) {
    CuMemFree (npp->in_fallback);
    npp->in_fallback = 0;
  }

  if (npp->out_fallback) {
    CuMemFree (npp->out_fallback);
    npp->out_fallback = 0;
  }

  gst_cuda_context_pop ();

done:

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static GstCaps *
gst_npp_scale_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *ret;
  GstStructure *structure;
  GstCapsFeatures *features;
  gint i, n;

  GST_DEBUG_OBJECT (trans,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  ret = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (caps, i);
    features = gst_caps_get_features (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure_full (ret, structure, features))
      continue;

    /* make copy */
    structure = gst_structure_copy (structure);

    gst_structure_set (structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);

    /* if pixel aspect ratio, make a range of it */
    if (gst_structure_has_field (structure, "pixel-aspect-ratio")) {
      gst_structure_set (structure, "pixel-aspect-ratio",
          GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1, NULL);
    }

    gst_caps_append_structure_full (ret, structure,
        gst_caps_features_copy (features));
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = intersection;
  }

  GST_DEBUG_OBJECT (trans, "returning caps: %" GST_PTR_FORMAT, ret);

  return ret;
}

static GstCaps *
gst_npp_scale_fixate_caps (GstBaseTransform * base, GstPadDirection direction,
    GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  const GValue *from_par, *to_par;
  GValue fpar = { 0, }, tpar = {
  0,};

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  from_par = gst_structure_get_value (ins, "pixel-aspect-ratio");
  to_par = gst_structure_get_value (outs, "pixel-aspect-ratio");

  /* If we're fixating from the sinkpad we always set the PAR and
   * assume that missing PAR on the sinkpad means 1/1 and
   * missing PAR on the srcpad means undefined
   */
  if (direction == GST_PAD_SINK) {
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION_RANGE);
      gst_value_set_fraction_range_full (&tpar, 1, G_MAXINT, G_MAXINT, 1);
      to_par = &tpar;
    }
  } else {
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&tpar, 1, 1);
      to_par = &tpar;

      gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
    }
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
  }

  /* we have both PAR but they might not be fixated */
  {
    gint from_w, from_h, from_par_n, from_par_d, to_par_n, to_par_d;
    gint w = 0, h = 0;
    gint from_dar_n, from_dar_d;
    gint num, den;

    /* from_par should be fixed */
    g_return_val_if_fail (gst_value_is_fixed (from_par), othercaps);

    from_par_n = gst_value_get_fraction_numerator (from_par);
    from_par_d = gst_value_get_fraction_denominator (from_par);

    gst_structure_get_int (ins, "width", &from_w);
    gst_structure_get_int (ins, "height", &from_h);

    gst_structure_get_int (outs, "width", &w);
    gst_structure_get_int (outs, "height", &h);

    /* if both width and height are already fixed, we can't do anything
     * about it anymore */
    if (w && h) {
      guint n, d;

      GST_DEBUG_OBJECT (base, "dimensions already set to %dx%d, not fixating",
          w, h);
      if (!gst_value_is_fixed (to_par)) {
        if (gst_video_calculate_display_ratio (&n, &d, from_w, from_h,
                from_par_n, from_par_d, w, h)) {
          GST_DEBUG_OBJECT (base, "fixating to_par to %dx%d", n, d);
          if (gst_structure_has_field (outs, "pixel-aspect-ratio"))
            gst_structure_fixate_field_nearest_fraction (outs,
                "pixel-aspect-ratio", n, d);
          else if (n != d)
            gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                n, d, NULL);
        }
      }
      goto done;
    }

    /* Calculate input DAR */
    if (!gst_util_fraction_multiply (from_w, from_h, from_par_n, from_par_d,
            &from_dar_n, &from_dar_d)) {
      GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
          ("Error calculating the output scaled size - integer overflow"));
      goto done;
    }

    GST_DEBUG_OBJECT (base, "Input DAR is %d/%d", from_dar_n, from_dar_d);

    /* If either width or height are fixed there's not much we
     * can do either except choosing a height or width and PAR
     * that matches the DAR as good as possible
     */
    if (h) {
      GstStructure *tmp;
      gint set_w, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (base, "height is fixed (%d)", h);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the width that is nearest to the
       * width with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (base, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        w = (guint) gst_util_uint64_scale_int_round (h, num, den);
        gst_structure_fixate_field_nearest_int (outs, "width", w);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input width */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "width", G_TYPE_INT, set_w,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the width to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int_round (h, num, den);
      gst_structure_fixate_field_nearest_int (outs, "width", w);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (w) {
      GstStructure *tmp;
      gint set_h, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (base, "width is fixed (%d)", w);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the height that is nearest to the
       * height with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (base, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        h = (guint) gst_util_uint64_scale_int_round (w, den, num);
        gst_structure_fixate_field_nearest_int (outs, "height", h);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input height */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }
      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "height", G_TYPE_INT, set_h,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the height to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      h = (guint) gst_util_uint64_scale_int_round (w, den, num);
      gst_structure_fixate_field_nearest_int (outs, "height", h);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (gst_value_is_fixed (to_par)) {
      GstStructure *tmp;
      gint set_h, set_w, f_h, f_w;

      to_par_n = gst_value_get_fraction_numerator (to_par);
      to_par_d = gst_value_get_fraction_denominator (to_par);

      /* Calculate scale factor for the PAR change */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_n,
              to_par_d, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      /* Try to keep the input height (because of interlacing) */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      w = (guint) gst_util_uint64_scale_int_round (set_h, num, den);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &set_w);
      gst_structure_free (tmp);

      /* We kept the DAR and the height is nearest to the original height */
      if (set_w == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      f_h = set_h;
      f_w = set_w;

      /* If the former failed, try to keep the input width at least */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      h = (guint) gst_util_uint64_scale_int_round (set_w, den, num);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_free (tmp);

      /* We kept the DAR and the width is nearest to the original width */
      if (set_h == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      /* If all this failed, keep the dimensions with the DAR that was closest
       * to the correct DAR. This changes the DAR but there's not much else to
       * do here.
       */
      if (set_w * ABS (set_h - h) < ABS (f_w - w) * f_h) {
        f_h = set_h;
        f_w = set_w;
      }
      gst_structure_set (outs, "width", G_TYPE_INT, f_w, "height", G_TYPE_INT,
          f_h, NULL);
      goto done;
    } else {
      GstStructure *tmp;
      gint set_h, set_w, set_par_n, set_par_d, tmp2;

      /* width, height and PAR are not fixed but passthrough is not possible */

      /* First try to keep the height and width as good as possible
       * and scale PAR */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);

        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* Otherwise try to scale width to keep the DAR with the set
       * PAR and height */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int_round (set_h, num, den);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, tmp2, "height",
            G_TYPE_INT, set_h, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* ... or try the same with the height */
      h = (guint) gst_util_uint64_scale_int_round (set_w, den, num);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, tmp2, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* If all fails we can't keep the DAR and take the nearest values
       * for everything from the first try */
      gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
          G_TYPE_INT, set_h, NULL);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);
    }
  }

done:
  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  if (from_par == &fpar)
    g_value_unset (&fpar);
  if (to_par == &tpar)
    g_value_unset (&tpar);

  return othercaps;
}

static NppiInterpolationMode
gst_npp_scale_scale_mode_to_npp (GstNppScale * npp)
{
  NppiInterpolationMode ret = NPPI_INTER_UNDEFINED;

  switch (npp->scale_mode) {
    case GST_NPP_SCALE_NN:
      return NPPI_INTER_NN;
    case GST_NPP_SCALE_LINEAR:
      return NPPI_INTER_LINEAR;
    case GST_NPP_SCALE_CUBIC:
      return NPPI_INTER_CUBIC;
    case GST_NPP_SCALE_CUBIC2P_BSPLINE:
      return NPPI_INTER_CUBIC2P_BSPLINE;
    case GST_NPP_SCALE_CUBIC2P_CATMULLROM:
      return NPPI_INTER_CUBIC2P_CATMULLROM;
    case GST_NPP_SCALE_CUBIC2P_B05C03:
      return NPPI_INTER_CUBIC2P_B05C03;
    case GST_NPP_SCALE_SUPER:
      return NPPI_INTER_SUPER;
    case GST_NPP_SCALE_LANCZOS:
      return NPPI_INTER_LANCZOS;
    case GST_NPP_SCALE_LANCZOS3_ADVANCED:
      return NPPI_INTER_LANCZOS3_ADVANCED;
    default:
      ret = NPPI_INTER_UNDEFINED;
      break;
  }

  return ret;
}

static gboolean
gst_npp_scale_configure (GstNppScale * npp, GstVideoInfo * in_info,
    GstVideoInfo * out_info)
{
  GstCudaBaseFilter *filter = GST_CUDA_BASE_FILTER (npp);
  gint i;
  gboolean found_final_stage = FALSE;

  if (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (npp)))
    return TRUE;

  npp->npp_interp_mode = gst_npp_scale_scale_mode_to_npp (npp);

  if (G_UNLIKELY (npp->npp_interp_mode == NPPI_INTER_UNDEFINED)) {
    GST_ERROR_OBJECT (npp, "undefined resize mode");
    return FALSE;
  }

  if (!gst_cuda_context_push (filter->context)) {
    GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
        ("Cannot push CUDA context"));
    return FALSE;
  }

  /* setup deinterleaving stage */
  gst_video_info_set_format (&npp->stage[NPP_STAGE_DEINTERLEAVE].in_info,
      GST_VIDEO_INFO_FORMAT (in_info), GST_VIDEO_INFO_WIDTH (in_info),
      GST_VIDEO_INFO_HEIGHT (in_info));

  gst_video_info_set_format (&npp->stage[NPP_STAGE_DEINTERLEAVE].out_info,
      GST_VIDEO_FORMAT_I420, GST_VIDEO_INFO_WIDTH (in_info),
      GST_VIDEO_INFO_HEIGHT (in_info));

  if (GST_VIDEO_INFO_N_PLANES (in_info) ==
      GST_VIDEO_INFO_N_COMPONENTS (in_info)) {
    npp->stage[NPP_STAGE_DEINTERLEAVE].need_process = FALSE;
  } else {
    npp->stage[NPP_STAGE_DEINTERLEAVE].need_process = TRUE;
  }
  npp->stage[NPP_STAGE_DEINTERLEAVE].final_stage = FALSE;

  /* setup interleaving stage */
  gst_video_info_set_format (&npp->stage[NPP_STAGE_INTERLEAVE].in_info,
      GST_VIDEO_FORMAT_I420, GST_VIDEO_INFO_WIDTH (out_info),
      GST_VIDEO_INFO_HEIGHT (out_info));

  gst_video_info_set_format (&npp->stage[NPP_STAGE_INTERLEAVE].out_info,
      GST_VIDEO_INFO_FORMAT (out_info), GST_VIDEO_INFO_WIDTH (out_info),
      GST_VIDEO_INFO_HEIGHT (out_info));

  if (GST_VIDEO_INFO_N_PLANES (out_info) ==
      GST_VIDEO_INFO_N_COMPONENTS (out_info)) {
    npp->stage[NPP_STAGE_INTERLEAVE].need_process = FALSE;
  } else {
    npp->stage[NPP_STAGE_INTERLEAVE].need_process = TRUE;
  }
  npp->stage[NPP_STAGE_INTERLEAVE].final_stage = FALSE;

  /* setup resize stage */
  gst_video_info_set_format (&npp->stage[NPP_STAGE_RESIZE].in_info,
      GST_VIDEO_FORMAT_I420, GST_VIDEO_INFO_WIDTH (in_info),
      GST_VIDEO_INFO_HEIGHT (in_info));

  gst_video_info_set_format (&npp->stage[NPP_STAGE_RESIZE].out_info,
      GST_VIDEO_FORMAT_I420, GST_VIDEO_INFO_WIDTH (out_info),
      GST_VIDEO_INFO_HEIGHT (out_info));

  if (GST_VIDEO_INFO_WIDTH (in_info) == GST_VIDEO_INFO_WIDTH (out_info) &&
      GST_VIDEO_INFO_HEIGHT (in_info) == GST_VIDEO_INFO_HEIGHT (out_info)) {
    npp->stage[NPP_STAGE_RESIZE].need_process = FALSE;
  } else {
    npp->stage[NPP_STAGE_RESIZE].need_process = TRUE;
  }
  npp->stage[NPP_STAGE_RESIZE].final_stage = FALSE;

  /* cleanup internal pool */
  if (npp->in_fallback)
    CuMemFree (npp->in_fallback);
  if (npp->out_fallback)
    CuMemFree (npp->out_fallback);

  if (!gst_cuda_result (CuMemAlloc (&npp->in_fallback,
              GST_VIDEO_INFO_SIZE (in_info)))) {
    GST_ERROR_OBJECT (npp, "Couldn't alloc input fallback memory");
    return FALSE;
  }

  if (!gst_cuda_result (CuMemAlloc (&npp->out_fallback,
              GST_VIDEO_INFO_SIZE (out_info)))) {
    GST_ERROR_OBJECT (npp, "Couldn't alloc output fallback memory");
    return FALSE;
  }

  for (i = G_N_ELEMENTS (npp->stage) - 1; i >= 0; i--) {
    CUresult rst;

    if (!npp->stage[i].need_process)
      continue;

    /* output memory alloc is not required for the final stage */
    if (!found_final_stage) {
      found_final_stage = TRUE;
      npp->stage[i].final_stage = TRUE;
      continue;
    }

    rst = CuMemAlloc (&npp->stage[i].out_buf,
        GST_VIDEO_INFO_SIZE (&npp->stage[i].out_info));

    if (!gst_cuda_result (rst)) {
      GST_ERROR_OBJECT (npp, "Couldn't allocated %dth stage memory", i);
      return FALSE;
    }
  }

  if (!gst_cuda_context_pop ()) {
    GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
        ("Cannot pop CUDA context"));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_npp_scale_set_info (GstCudaBaseFilter * filter, GstCaps * incaps,
    GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info)
{
  GstNppScale *npp = GST_NPP_SCALE (filter);

  if (GST_VIDEO_INFO_WIDTH (in_info) == GST_VIDEO_INFO_WIDTH (out_info) &&
      GST_VIDEO_INFO_HEIGHT (in_info) == GST_VIDEO_INFO_HEIGHT (out_info) &&
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_INFO_FORMAT (out_info)) {
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (filter), TRUE);

    return TRUE;
  }

  if (!gst_npp_scale_configure (npp, in_info, out_info)) {
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_npp_scale_deinterleave (GstNppScale * npp, NppStageData * stage,
    CUdeviceptr in_buf, CUdeviceptr out_buf)
{
  NppStatus ret;
  int nSrcYStep;
  const Npp8u *pSrcCbCr;
  int nSrcCbCrStep;
  Npp8u *pDst[3];
  int rDstStep[3];
  NppiSize oSizeROI;

  pSrcCbCr =
      (const Npp8u *) in_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->in_info, 1);

  nSrcYStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, 0);
  nSrcCbCrStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, 1);

  pDst[0] = (Npp8u *) out_buf;
  pDst[1] =
      (Npp8u *) out_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->out_info, 1);
  pDst[2] =
      (Npp8u *) out_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->out_info, 2);

  rDstStep[0] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, 0);
  rDstStep[1] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, 1);
  rDstStep[2] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, 2);

  oSizeROI.width = GST_VIDEO_INFO_WIDTH (&stage->in_info);
  oSizeROI.height = GST_VIDEO_INFO_HEIGHT (&stage->in_info);

  ret = NppiYCbCr420_8u_P2P3R ((const Npp8u * const) in_buf, nSrcYStep,
      pSrcCbCr, nSrcCbCrStep, pDst, rDstStep, oSizeROI);

  if (ret != NPP_SUCCESS)
    return FALSE;

  return TRUE;
}

static gboolean
gst_npp_scale_resize (GstNppScale * npp, NppStageData * stage,
    CUdeviceptr in_buf, CUdeviceptr out_buf)
{
  gint i;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (&stage->out_info); i++) {
    const Npp8u *pSrc;
    Npp8u *pDst;
    int nSrcStep;
    int nDstStep;
    NppiSize in_size;
    NppiRect in_rect = { 0, };
    NppiRect out_rect = { 0, };
    NppStatus ret;

    pSrc =
        (const Npp8u *) in_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->in_info,
        i);
    pDst =
        (Npp8u *) out_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->out_info, i);

    nSrcStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, i);
    nDstStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, i);

    in_rect.width = in_size.width =
        GST_VIDEO_INFO_COMP_WIDTH (&stage->in_info, i) *
        GST_VIDEO_INFO_COMP_PSTRIDE (&stage->in_info, i);
    in_rect.height = in_size.height =
        GST_VIDEO_INFO_COMP_HEIGHT (&stage->in_info, i);

    out_rect.width = GST_VIDEO_INFO_COMP_WIDTH (&stage->out_info, i) *
        GST_VIDEO_INFO_COMP_PSTRIDE (&stage->out_info, i);
    out_rect.height = GST_VIDEO_INFO_COMP_HEIGHT (&stage->out_info, i);

    ret = NppiResizeSqrPixel_8u_C1R (pSrc, in_size, nSrcStep, in_rect,
        pDst, nDstStep, out_rect, (double) out_rect.width / in_rect.width,
        (double) out_rect.height / in_rect.height, 0.0, 0.0,
        npp->npp_interp_mode);

    if (ret != NPP_SUCCESS)
      return FALSE;
  }

  return TRUE;
}

static gboolean
gst_npp_scale_interleave (GstNppScale * npp, NppStageData * stage,
    CUdeviceptr in_buf, CUdeviceptr out_buf)
{
  NppStatus ret;
  const Npp8u *pSrc[3];
  int rSrcStep[3];
  Npp8u *pDstY;
  int nDstYStep;
  Npp8u *pDstCbCr;
  int nDstCbCrStep;
  NppiSize oSizeROI;

  pSrc[0] = (Npp8u *) in_buf;
  pSrc[1] = (Npp8u *) in_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->in_info, 1);
  pSrc[2] = (Npp8u *) in_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->in_info, 2);

  rSrcStep[0] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, 0);
  rSrcStep[1] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, 1);
  rSrcStep[2] = GST_VIDEO_INFO_PLANE_STRIDE (&stage->in_info, 2);

  pDstY = (Npp8u *) out_buf;
  pDstCbCr =
      (Npp8u *) out_buf + GST_VIDEO_INFO_PLANE_OFFSET (&stage->out_info, 1);

  nDstYStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, 0);
  nDstCbCrStep = GST_VIDEO_INFO_PLANE_STRIDE (&stage->out_info, 1);

  oSizeROI.width = GST_VIDEO_INFO_WIDTH (&stage->in_info);
  oSizeROI.height = GST_VIDEO_INFO_HEIGHT (&stage->in_info);

  GST_LOG_OBJECT (npp,
      "Interleave src step %d:%d:%d, dstYStep %d, dstCbCrStep %d",
      rSrcStep[0], rSrcStep[1], rSrcStep[2], nDstYStep, nDstCbCrStep);

  ret = NppiYCbCr420_8u_P3P2R (pSrc, rSrcStep,
      pDstY, nDstYStep, pDstCbCr, nDstCbCrStep, oSizeROI);

  if (ret != NPP_SUCCESS) {
    GST_ERROR_OBJECT (npp, "Couldn't interleave frame, NppStatus: %d", ret);
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_npp_scale_transform_frame (GstCudaBaseFilter * filter,
    GstCudaMemoryTarget in_target, GstVideoFrame * in_frame,
    GstCudaMemoryTarget out_target, GstVideoFrame * out_frame)
{
  GstNppScale *npp = GST_NPP_SCALE (filter);
  gint i;
  CUdeviceptr in_ptr, out_ptr;
  CUdeviceptr processed_ptr;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!gst_cuda_context_push (filter->context)) {
    GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
        ("Cannot push CUDA context"));
    return GST_FLOW_ERROR;
  }

  in_ptr = out_ptr = 0;

  if (in_target == GST_CUDA_MEMORY_TARGET_DEVICE) {
    in_ptr = (CUdeviceptr) in_frame->data[0];
  } else {
    /* upload frame to device memory */
    for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (in_frame); i++) {
      gint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (in_frame, i);
      guint8 *src = GST_VIDEO_FRAME_PLANE_DATA (in_frame, i);
      CUDA_MEMCPY2D param = { 0, };
      guint width, height;

      width = GST_VIDEO_FRAME_COMP_WIDTH (in_frame, i) *
          GST_VIDEO_FRAME_COMP_PSTRIDE (in_frame, i);
      height = GST_VIDEO_FRAME_COMP_HEIGHT (in_frame, i);

      param.srcMemoryType = CU_MEMORYTYPE_HOST;
      param.srcPitch = src_stride;
      param.srcHost = src;
      param.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      param.dstPitch = GST_VIDEO_INFO_PLANE_STRIDE (&filter->in_info, i);
      param.dstDevice =
          npp->in_fallback + GST_VIDEO_INFO_PLANE_OFFSET (&filter->in_info, i);
      param.WidthInBytes = width;
      param.Height = height;

      if (!gst_cuda_result (CuMemcpy2D (&param))) {
        GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
            ("Cannot upload input video frame"));
        return GST_FLOW_ERROR;
      }
    }

    in_ptr = npp->in_fallback;
  }

  if (out_target == GST_CUDA_MEMORY_TARGET_DEVICE) {
    out_ptr = (CUdeviceptr) out_frame->data[0];
  } else {
    out_ptr = npp->out_fallback;
  }

  if (npp->stage[NPP_STAGE_DEINTERLEAVE].need_process) {
    if (npp->stage[NPP_STAGE_DEINTERLEAVE].final_stage)
      processed_ptr = out_ptr;
    else
      processed_ptr = npp->stage[NPP_STAGE_DEINTERLEAVE].out_buf;

    if (!gst_npp_scale_deinterleave (npp,
            &npp->stage[NPP_STAGE_DEINTERLEAVE], in_ptr, processed_ptr)) {
      GST_ERROR_OBJECT (npp, "Couldn't deinterleave input buffer");
      ret = GST_FLOW_ERROR;
      goto done;
    }

    if (!npp->stage[NPP_STAGE_DEINTERLEAVE].final_stage)
      in_ptr = processed_ptr;
  }

  if (npp->stage[NPP_STAGE_RESIZE].need_process) {
    if (npp->stage[NPP_STAGE_RESIZE].final_stage)
      processed_ptr = out_ptr;
    else
      processed_ptr = npp->stage[NPP_STAGE_RESIZE].out_buf;

    if (!gst_npp_scale_resize (npp,
            &npp->stage[NPP_STAGE_RESIZE], in_ptr, processed_ptr)) {
      GST_ERROR_OBJECT (npp, "Couldn't resize input buffer");
      ret = GST_FLOW_ERROR;
      goto done;
    }

    if (!npp->stage[NPP_STAGE_RESIZE].final_stage)
      in_ptr = processed_ptr;
  }

  if (npp->stage[NPP_STAGE_INTERLEAVE].need_process) {
    if (!gst_npp_scale_interleave (npp,
            &npp->stage[NPP_STAGE_INTERLEAVE], in_ptr, out_ptr)) {
      ret = GST_FLOW_ERROR;
      goto done;
    }
  }

  if (out_ptr == npp->out_fallback) {
    for (i = 0; i < GST_VIDEO_FRAME_N_PLANES (out_frame); i++) {
      CUDA_MEMCPY2D param = { 0, };
      gint src_stride = GST_VIDEO_INFO_PLANE_STRIDE (&filter->out_info, i);
      guint width, height;

      width = GST_VIDEO_FRAME_COMP_WIDTH (out_frame, i) *
          GST_VIDEO_FRAME_COMP_PSTRIDE (out_frame, i);
      height = GST_VIDEO_FRAME_COMP_HEIGHT (out_frame, i);

      param.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      param.srcPitch = src_stride;
      param.srcDevice =
          out_ptr + GST_VIDEO_INFO_PLANE_OFFSET (&filter->out_info, i);
      param.dstMemoryType = CU_MEMORYTYPE_HOST;
      param.dstPitch = GST_VIDEO_FRAME_PLANE_STRIDE (out_frame, i);
      param.dstHost = GST_VIDEO_FRAME_PLANE_DATA (out_frame, i);
      param.WidthInBytes = width;
      param.Height = height;

      if (!gst_cuda_result (CuMemcpy2D (&param))) {
        GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
            ("Cannot upload input video frame"));
        return GST_FLOW_ERROR;
      }
    }
  }

done:
  if (!gst_cuda_context_pop ()) {
    GST_ELEMENT_ERROR (npp, LIBRARY, FAILED, (NULL),
        ("Cannot pop CUDA context"));
    return GST_FLOW_ERROR;
  }

  return ret;
}
