AC_INIT([watchman], [2.3.1], [], [watchman])
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])

dnl we really want you to be using 64 hardware and software
CPPFLAGS="$CPPFLAGS -D_REENTRANT"
CFLAGS="$CFLAGS -m64"

AC_PROG_CC
AC_PROG_CPP
AC_C_BIGENDIAN
AC_C_INLINE
AC_PROG_RANLIB

AM_PROG_CC_C_O
AM_PROG_AS

dnl Look for "arc" so that we can conditionally enable more pedantry
dnl when building.  We prefer that contributors use "arc" but don't
dnl require users to have it to build and use watchman
AC_CHECK_PROG(HAVE_ARC, arc, yes, no)
AM_CONDITIONAL(HAVE_ARC, test "x$HAVE_ARC" = xyes)

dnl I don't care that this is supposed to be sysconfdir; autoconf doesn't
dnl allow it to be evaluated sanely from configure, only via Makefile,
dnl which I don't care about.  Just pass the right value to configure.
conffile=/etc/watchman.json
AC_ARG_ENABLE(conffile, [
  --enable-conffile=PATH  Use PATH as the default configuration file name.
                          Default is $conffile
],[
  conffile=$enableval
])
if test "$conffile" != "no" ; then
  AC_DEFINE_UNQUOTED(WATCHMAN_CONFIG_FILE, "$conffile",
                     [system configuration file path])
fi

AC_ARG_ENABLE(statedir, [
  --enable-statedir=PATH  Use PATH as the default for state, log files
                          and sockets instead of using your system tempdir
],[
 AC_DEFINE_UNQUOTED(WATCHMAN_STATE_DIR, "$enableval",
                    [default location for state])
])

pcre_config="yes"
AC_ARG_WITH(pcre, [
  --without-pcre       Don't enable pcre support.
  --with-pcre=PATH     Enable pcre support.  PATH is location of pcre-config.
                       Default is to enable and look for pcre-config in your
                       $PATH
],[
  pcre_config="$withval"
])

if test "$pcre_config" != "no" ; then
  if test "$pcre_config" == "yes" ; then
    pcre_config="pcre-config"
  fi

  if $pcre_config --version >/dev/null ; then
    LIBS="$LIBS `$pcre_config --libs`"
    CFLAGS="$CFLAGS `$pcre_config --cflags`"
    AC_CHECK_HEADERS(pcre.h)
  fi
fi


gimli=no
AC_ARG_WITH(gimli, [
  --with-gimli    Enable support for the gimli process monitor
                  https://bitbucket.org/wez/gimli/
],[
   gimli=$withval
])
if test "x$gimli" != "xno" ; then
  AC_DEFINE([USE_GIMLI], 1, [Use gimli])

  dnl deal with annoying lack of rpath and linux lib64
  dirs="/usr/local/lib /usr/lib"
  rpath="-R"
  case $target_os-$target_cpu in
    linux-*64)
      dirs="/usr/local/lib64 $libs"
      rpath="-Wl,-rpath="
      ;;
    linux*)
      rpath="-Wl,-rpath="
      ;;
  esac
  save_flags="$LDFLAGS"
  save_libs="$LIBS"
  gimli_dir=""
  MONITOR_PATH="monitor"
  for dir in $dirs ; do
    LDFLAGS="$save_flags $rpath$dir -L$dir"
    LIBS="$save_libs -lgimli"
    AC_TRY_LINK([
#include <libgimli.h>
],[gimli_heartbeat_attach();],
      [gimli_dir=$dir]
    )
    if test -n "$gimli_dir" ; then
      save_flags="$save_flags $rpath$dir -L$dir"
      save_libs="$save_libs -lgimli"
      dnl resolve real path to bin dir
      MONITOR_PATH="`cd $gimli_dir/../bin ; echo \$PWD`/monitor"
      break
    fi
  done
  if test ! -x "$MONITOR_PATH" ; then
    MONITOR_PATH="monitor"
  fi
  AC_DEFINE_UNQUOTED(GIMLI_MONITOR_PATH,
                     "$MONITOR_PATH",
                     [path to gimli monitor process])
  LDFLAGS="$save_flags"
  LIBS="$save_libs"
  AC_CHECK_HEADERS(libgimli.h)
fi

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
AC_SEARCH_LIBS([socket], [socket])

AC_CHECK_HEADERS(sys/types.h inttypes.h locale.h port.h sys/inotify.h sys/event.h)
AC_CHECK_FUNCS(kqueue port_create inotify_init strtoll localeconv)
AC_CHECK_HEADERS(valgrind/valgrind.h)

# Do this after we've looked for functions
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -Wall -Wextra -Wdeclaration-after-statement -gdwarf-2 -fno-omit-frame-pointer"
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

