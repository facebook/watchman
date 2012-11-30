AC_INIT([watchman], [1.0], [], [watchman])
AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects -Wall])
AC_PROG_CC
AC_C_BIGENDIAN

AM_PROG_CC_C_O
AM_PROG_AS

AC_SEARCH_LIBS(
  [event_init],
  [event event-1.4 event2],
  [found_libevent=yes],
  [found_libevent=no]
)

AC_CHECK_HEADERS(event.h sys/inotify.h sys/event.h)

AC_CHECK_FUNCS(kqueue inotify_init)

if test "x$found_libevent" = "xno" ; then
  AC_MSG_ERROR("we need libevent")
fi
AC_CONFIG_HEADER([watchman-config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

dnl vim:ts=2:sw=2:

