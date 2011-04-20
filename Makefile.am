ACLOCAL_AMFLAGS = -I m4
SUBDIRS = display/balloon display/default display/nico2 # subscribe/feed

bin_PROGRAMS = gol
gol_SOURCES = gol.c gol.h
gol_CFLAGS = $(GTK2_CFLAGS) $(OPENSSL_CFLAGS) $(SQLITE3_CFLAGS)
gol_LDADD = $(GTK2_LIBS) $(OPENSSL_LIBS) $(SQLITE3_LIBS)

EXTRA_DIST = data gol.rc Makefile.w32 README.mkd TODO
