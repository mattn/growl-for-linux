ACLOCAL_AMFLAGS = -I m4
SUBDIRS = plugins data \
		  display/balloon display/fog display/nico2 $(ENABLE_LIBNOTIFY) \
		  subscribe/tweets subscribe/rhythmbox

bin_PROGRAMS = gol
gol_SOURCES = gol.c gol.h compatibility.h
gol_CFLAGS = $(GTHREAD2_CFLAGS) $(GTK2_CFLAGS) $(OPENSSL_CFLAGS) $(SQLITE3_CFLAGS) $(APP_INDICATOR_CFLAGS) -DDATADIR='"$(pkgdatadir)"' -DLIBDIR='"$(pkglibdir)"'
gol_LDADD = $(GTHREAD2_LIBS) $(GTK2_LIBS) $(OPENSSL_LIBS) $(SQLITE3_LIBS) $(GMODULE2_LIBS) $(APP_INDICATOR_LIBS)

EXTRA_DIST = gol.rc Makefile.w32 README.mkd TODO data/gol.desktop VERSION

install-data-local: data/gol.desktop
	mkdir -p $(DESTDIR)/$(datadir)/applications
	desktop-file-install --dir=$(DESTDIR)/$(datadir)/applications data/gol.desktop

uninstall-local:
	rm -f $(DESTDIR)/$(datadir)/applications/gol.desktop
