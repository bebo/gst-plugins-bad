plugin_LTLIBRARIES = libgstnpp.la

libgstnpp_la_SOURCES = \
	plugin.c \
	gstnpploader.c \
	gstnppscale.c

noinst_HEADERS = \
	gstnpploader.h \
	gstnppscale.h

libgstnpp_la_CFLAGS = \
	$(GST_PLUGINS_BAD_CFLAGS) \
	$(GST_GL_CFLAGS) \
	$(GST_PBUTILS_CFLAGS) \
	$(GST_VIDEO_CFLAGS) \
	$(GST_CFLAGS) \
	$(CUDA_CFLAGS)

libgstnpp_la_LIBADD = \
	$(GST_GL_LIBS) \
	$(GST_PBUTILS_LIBS) \
	$(GST_VIDEO_LIBS) \
	$(GST_LIBS) \
	$(top_builddir)/gst-libs/gst/cuda/libgstcuda-$(GST_API_VERSION).la

libgstnpp_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

