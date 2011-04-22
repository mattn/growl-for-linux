subscribedir = $(pkglibdir)/subscribe
subscribe_LTLIBRARIES = libtweets.la
libtweets_la_SOURCES = tweets.c
libtweets_la_LDFLAGS = -rpath $(subscribedir) -module
libtweets_la_CFLAGS = $(GLIB2_CFLAGS) $(LIBCURL_CFLAGS) $(LIBXML2_CFLAGS)
libtweets_la_LIBADD = $(GLIB2_LIBS) $(LIBCURL_LIBS) $(LIBXML2_LIBS)

EXTRA_DIST = DO_NOT_USE_THIS_MODULE Makefile.w32
