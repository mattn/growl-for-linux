lib_LTLIBRARIES = libdefault.la
libdefault_la_SOURCES = default.c
libdefault_la_LDFLAGS = -module -version-info 0:0:0
libdefault_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libdefault_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = display_default.xpm Makefile.w32
