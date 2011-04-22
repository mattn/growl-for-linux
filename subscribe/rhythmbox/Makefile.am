subscribedir = $(pkglibdir)/subscribe
subscribe_LTLIBRARIES = librhythmbox.la
librhythmbox_la_SOURCES = rhythmbox.c
librhythmbox_la_LDFLAGS = -rpath $(subscribedir) -module
librhythmbox_la_CFLAGS = $(GLIB2_CFLAGS) $(LIBCURL_CFLAGS) $(LIBXML2_CFLAGS) $(DBUSGLIB1_CFLAGS)
librhythmbox_la_LIBADD = $(GLIB2_LIBS) $(LIBCURL_LIBS) $(LIBXML2_LIBS) $(DBUSGLIB1_LIBS)

EXTRA_DIST = 
