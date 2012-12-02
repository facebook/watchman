AC_INIT([watchman], [1.0], [], [watchman])
AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])
AC_PROG_CC
AC_C_BIGENDIAN
AC_C_INLINE
AC_PROG_RANLIB

AM_PROG_CC_C_O
AM_PROG_AS

dnl could do some feature testing here to turn on a safe set
dnl of warnings, and also add an option to turn on -Werror
dnl for maintainers.
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -Wall -Wextra -Wdeclaration-after-statement"
fi

lenient=no
AC_ARG_ENABLE(lenient, [
  --enable-lenient  Turn off more pedantic levels of warnings
                    and compilation checks
],[
   lenient=$enableval
])
if test -n "$GCC" -a "$lenient" == "no" ; then
   CFLAGS="$CFLAGS -Werror"
fi

AC_SEARCH_LIBS([pthread_create], [pthread])

AC_CHECK_HEADERS(locale.h sys/inotify.h sys/event.h)
AC_CHECK_FUNCS(kqueue inotify_init strtoll localeconv)

AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile thirdparty/jansson/jansson_config.h])
AC_OUTPUT

dnl vim:ts=2:sw=2:

