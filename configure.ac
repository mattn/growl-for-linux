AC_PREREQ([2.63])
AC_INIT([growl-for-linux], m4_esyscmd([cat VERSION | tr -d '\n']), [mattn.jp@gmail.com])
AC_CONFIG_SRCDIR([gol.c])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign])

# Smart output for automake.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Checks for programs.
AC_PROG_CC_C99
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AM_PROG_CC_C_O

# Enable -Werror.
AC_ARG_ENABLE([werror], [AS_HELP_STRING([--enable-werror], [enable -Werror])], [werror_flag=yes])
case ${werror_flag} in
    yes) CFLAGS="-Werror ${CFLAGS}";;
esac

# Checks for libraries.

PKG_CHECK_MODULES(GLIB2, glib-2.0)
PKG_CHECK_MODULES(GTHREAD2, gthread-2.0)
PKG_CHECK_MODULES(GTK2, gtk+-2.0)
PKG_CHECK_MODULES(GMODULE2, gmodule-2.0)
PKG_CHECK_MODULES(LIBCURL, libcurl)
PKG_CHECK_MODULES(LIBXML2, libxml-2.0)
PKG_CHECK_MODULES(OPENSSL, openssl)
PKG_CHECK_MODULES(SQLITE3, sqlite3)
PKG_CHECK_MODULES(DBUSGLIB1, dbus-glib-1)
PKG_CHECK_MODULES(NOTIFY, libnotify,
                  [ENABLE_LIBNOTIFY="display/libnotify"
                   AC_SUBST([ENABLE_LIBNOTIFY])
                   AC_CONFIG_FILES([display/libnotify/Makefile])],
                  [ENABLE_LIBNOTIFY=""])

APPINDICATOR_REQUIRED=0.4.92
AC_ARG_ENABLE(appindicator,
              AS_HELP_STRING([--enable-appindicator[=@<:@no/auto/yes@:>@]],
                             [Build support for application indicators ]),
              [enable_appindicator=$enableval],
              [enable_appindicator="auto"])
if test x$enable_appindicator = xauto ; then
        PKG_CHECK_EXISTS([appindicator-0.1 >= $APPINDICATOR_REQUIRED],
                         enable_appindicator="yes",
                         enable_appindicator="no")
fi
if test x$enable_appindicator = xyes ; then
        PKG_CHECK_EXISTS([appindicator-0.1 >= $APPINDICATOR_REQUIRED],,
                         AC_MSG_ERROR([appindicator-0.1 is not installed]))
        PKG_CHECK_MODULES(APP_INDICATOR,
                          appindicator-0.1 >= $APPINDICATOR_REQUIRED)
        AC_DEFINE(HAVE_APP_INDICATOR, [1], [Have AppIndicator])
fi
AM_CONDITIONAL(HAVE_APP_INDICATOR, test x"$enable_appindicator" = xyes)

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h memory.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE
AC_C_RESTRICT
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_TYPES([ptrdiff_t])

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([inet_ntoa memset socket strcasecmp strchr strncasecmp strndup strpbrk strstr strtol])

AC_CONFIG_FILES([Makefile
                 plugins/Makefile
                 data/Makefile
                 display/balloon/Makefile
                 display/fog/Makefile
                 display/nico2/Makefile
                 subscribe/tweets/Makefile
                 subscribe/rhythmbox/Makefile
                 data/gol.desktop])
AC_OUTPUT
