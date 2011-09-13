displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libdefault.la
libdefault_la_SOURCES = default.c display_default.xpm
libdefault_la_LDFLAGS = -L../../plugins
libdefault_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libdefault_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS) -lgolplug

EXTRA_DIST = Makefile.w32
