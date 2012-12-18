AC_INIT([watchman], [1.0], [], [watchman])
AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])
AC_PROG_CC
AC_C_BIGENDIAN
AC_C_INLINE
AC_PROG_RANLIB

AM_PROG_CC_C_O
AM_PROG_AS

lenient=no
AC_ARG_ENABLE(lenient, [
  --enable-lenient  Turn off more pedantic levels of warnings
                    and compilation checks
],[
   lenient=$enableval
])

stack_protect=no
AC_ARG_ENABLE(stack-protector, [
  --enable-stack-protector  Enable stack protection in the same
                            way that rpmbuild does on some systems.
],[
   stack_protect=$enableval
])

AC_SEARCH_LIBS([pthread_create], [pthread])

AC_CHECK_HEADERS(locale.h sys/inotify.h sys/event.h)
AC_CHECK_FUNCS(kqueue inotify_init strtoll localeconv)
AC_CHECK_HEADERS(valgrind/valgrind.h)

# Do this after we've looked for functions
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -Wall -Wextra -Wdeclaration-after-statement"
fi
if test -n "$GCC" -a "$stack_protect" == "yes" ; then
  CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4"
fi
IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" == "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile thirdparty/jansson/jansson_config.h])
AC_OUTPUT

dnl vim:ts=2:sw=2:

