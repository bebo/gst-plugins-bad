plugin_LTLIBRARIES = libgstnvcodec.la

libgstnvcodec_la_SOURCES = \
	plugin.c \
	gstcudadownload.c \
	gstcudaupload.c

if USE_NVDEC
libgstnvcodec_la_SOURCES += \
	gstnvdec.c \
	gstcuvidloader.c
endif

if USE_NVENC
libgstnvcodec_la_SOURCES += \
	gstnvenc.c \
	gstnvbaseenc.c \
	gstnvh264enc.c \
	gstnvh265enc.c
endif

noinst_HEADERS = \
	gstnvdec.h \
	gstnvenc.h \
	gstnvbaseenc.h \
	gstnvh264enc.h \
	gstnvh265enc.h \
	gstcuvidloader.h \
	gstcudadownload.h \
	gstcudaupload.h

libgstnvcodec_la_CFLAGS = \
	$(GST_PLUGINS_BAD_CFLAGS) \
	$(GST_GL_CFLAGS) \
	$(GST_PBUTILS_CFLAGS) \
	$(GST_VIDEO_CFLAGS) \
	$(GST_CFLAGS) \
	$(CUDA_CFLAGS)

libgstnvcodec_la_LIBADD = \
	$(GST_GL_LIBS) \
	$(GST_PBUTILS_LIBS) \
	$(GST_VIDEO_LIBS) \
	$(GST_LIBS) \
	$(GMODULE_NO_EXPORT_LIBS) \
	$(top_builddir)/gst-libs/gst/cuda/libgstcuda-$(GST_API_VERSION).la

libgstnvcodec_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

EXTRA_DIST = \
	nvEncodeAPI.h
