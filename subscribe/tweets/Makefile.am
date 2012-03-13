subscribedir = $(pkglibdir)/subscribe
subscribe_LTLIBRARIES = libtweets.la
libtweets_la_SOURCES = tweets.c
libtweets_la_LDFLAGS = -rpath $(subscribedir) -module -L../../plugins
libtweets_la_CFLAGS = $(GLIB2_CFLAGS) $(LIBCURL_CFLAGS) $(LIBXML2_CFLAGS) -I$(srcdir)/../..
libtweets_la_LIBADD = $(GLIB2_LIBS) $(LIBCURL_LIBS) $(LIBXML2_LIBS) -lgolplug

EXTRA_DIST = DO_NOT_USE_THIS_MODULE Makefile.w32
