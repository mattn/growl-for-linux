displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libnotify.la
libnotify_la_SOURCES = libnotify.c
libnotify_la_LDFLAGS = -L../../plugins
libnotify_la_CFLAGS = $(GTK2_CFLAGS) $(LIBCURL_CFLAGS) $(SQLITE3_CFLAGS)
libnotify_la_LIBADD = $(GTK2_LIBS) $(LIBCURL_LIBS) $(SQLITE3_LIBS) -lgolplug

EXTRA_DIST = Makefile.w32
