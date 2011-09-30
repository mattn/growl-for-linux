displaydir = $(pkglibdir)/display
display_LTLIBRARIES = libnotify_gol.la
libnotify_gol_la_SOURCES = notify.c display_libnotify.xpm
libnotify_gol_la_LDFLAGS = -L../../plugins
libnotify_gol_la_CFLAGS = $(GTK2_CFLAGS) $(LIBNOTIFY_CFLAGS)
libnotify_gol_la_LIBADD = $(GTK2_LIBS) $(LIBNOTIFY_LIBS) -lgolplug

EXTRA_DIST = Makefile.w32
