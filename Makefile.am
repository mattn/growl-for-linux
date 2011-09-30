ACLOCAL_AMFLAGS = -I m4
SUBDIRS = plugins data \
		  display/balloon display/default display/nico2 $(ENABLE_LIBNOTIFY) \
		  subscribe/tweets subscribe/rhythmbox

bin_PROGRAMS = gol
gol_SOURCES = gol.c gol.h
gol_CFLAGS = $(GTK2_CFLAGS) $(OPENSSL_CFLAGS) $(SQLITE3_CFLAGS) -DDATADIR='"$(pkgdatadir)"' -DLIBDIR='"$(pkglibdir)"'
gol_LDADD = $(GTK2_LIBS) $(OPENSSL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = gol.rc Makefile.w32 README.mkd TODO data/gol.desktop VERSION

install-data-local: data/gol.desktop
	mkdir -p $(DESTDIR)/$(datadir)/applications
	desktop-file-install --dir=$(DESTDIR)/$(datadir)/applications data/gol.desktop

uninstall-local:
	rm -f $(DESTDIR)/$(datadir)/applications/gol.desktop
