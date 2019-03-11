lib_LTLIBRARIES = libgstcuda-@GST_API_VERSION@.la

libgstcuda_@GST_API_VERSION@_la_SOURCES = \
	gstcudaloader.c \
	gstcudautils.c \
	gstcudacontext.c \
	gstcudamemory.c \
	gstcudabufferpool.c

noinst_HEADERS = \
	gstcudaloader.h \
	gstcuda_private.h

libgstcuda_@GST_API_VERSION@_la_CFLAGS = \
	$(GST_PLUGINS_BAD_CFLAGS) \
	$(GST_PLUGINS_BASE_CFLAGS) $(GST_BASE_CFLAGS) $(GST_CFLAGS) \
	-DBUILDING_GST_CUDA \
	$(GST_GL_CFLAGS) \
	$(CUDA_CFLAGS)

libgstcuda_@GST_API_VERSION@_la_LIBADD = $(GST_LIBS) $(GST_BASE_LIBS) \
	-lgstvideo-$(GST_API_VERSION)

libgstcuda_@GST_API_VERSION@_la_LDFLAGS = \
	$(GST_LIB_LDFLAGS) $(GST_ALL_LDFLAGS) $(GST_LT_LDFLAGS) $(GST_GL_LIBS)

libgstcuda_@GST_API_VERSION@includedir = \
	$(includedir)/gstreamer-@GST_API_VERSION@/gst/cuda
libgstcuda_@GST_API_VERSION@include_HEADERS = \
	gstcuda.h \
	gstcuda_fwd.h \
	cuda-prelude.h \
	gstcudautils.h \
	gstcudacontext.h \
	gstcudamemory.h \
	gstcudabufferpool.h
