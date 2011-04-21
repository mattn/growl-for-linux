AC_PREREQ([2.65])
AC_INIT([growl-for-linux], [0.0], [mattn.jp@gmail.com])
AC_CONFIG_SRCDIR([gol.c])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AM_PROG_CC_C_O

# Checks for libraries.

PKG_CHECK_MODULES(GLIB2, glib-2.0)
PKG_CHECK_MODULES(GTK2, gtk+-2.0)
PKG_CHECK_MODULES(LIBCURL, libcurl)
PKG_CHECK_MODULES(LIBXML2, libxml-2.0)
PKG_CHECK_MODULES(OPENSSL, openssl)
PKG_CHECK_MODULES(SQLITE3, sqlite3)

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h memory.h netdb.h netinet/in.h stdlib.h string.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([inet_ntoa memset socket strchr strncasecmp strpbrk strstr])

AC_CONFIG_FILES([Makefile
                 data/Makefile
                 display/balloon/Makefile
                 display/default/Makefile
                 display/nico2/Makefile
                 subscribe/feed/Makefile
                 data/gol.desktop])
AC_OUTPUT
