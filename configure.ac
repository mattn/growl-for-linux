#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.65])
AC_INIT([growl-for-linux], [0.0], [mattn.jp@gmail.com])
AC_CONFIG_SRCDIR([gol.c])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AM_PROG_CC_C_O

# Checks for libraries.

PKG_CHECK_MODULES(GOL, gtk+-2.0 openssl sqlite3)
PKG_CHECK_MODULES(GOL_DISPLAY, gtk+-2.0 libcurl sqlite3)

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
                 display/balloon/Makefile
                 display/default/Makefile
                 display/nico2/Makefile
                 subscribe/feed/Makefile])
AC_OUTPUT
