displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libnico2.la
libnico2_la_SOURCES = nico2.c display_nico2.xpm
libnico2_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libnico2_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = Makefile.w32
