lib_LTLIBRARIES = libballoon.la
libballoon_la_SOURCES = balloon.c
libballoon_la_LDFLAGS = -module -version-info 0:0:0
libballoon_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libballoon_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = balloon.xpm display_balloon.xpm Makefile.w32
