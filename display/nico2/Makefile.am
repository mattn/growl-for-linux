lib_LTLIBRARIES = libnico2.la
libnico2_la_SOURCES = nico2.c
libnico2_la_LDFLAGS = -module -version-info 0:0:0
libnico2_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libnico2_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = display_nico2.xpm Makefile.w32
