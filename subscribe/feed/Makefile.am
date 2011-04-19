lib_LTLIBRARIES = libfeed.la
libfeed_la_SOURCES = feed.c
libfeed_la_LDFLAGS = -module -version-info 0:0:0
libfeed_la_CFLAGS = $(GLIB2_CFLAGS) $(LIBCURL_CFLAGS) $(LIBXML2_CFLAGS)
libfeed_la_LIBADD = $(GLIB2_LIBS) $(LIBCURL_LIBS) $(LIBXML2_LIBS)

EXTRA_DIST = DO_NOT_USE_THIS_MODULE Makefile.w32
