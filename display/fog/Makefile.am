displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libfog.la
libfog_la_SOURCES = fog.c display_fog.xpm
libfog_la_LDFLAGS = -L../../plugins
libfog_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS) -I$(srcdir)/../..
libfog_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS) -lgolplug

EXTRA_DIST = Makefile.w32
