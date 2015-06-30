AC_INIT([watchman], [3.5.0], [], [watchman])
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES(yes)])

CPPFLAGS="$CPPFLAGS -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"
case $target_os in
  *solaris*)
    dnl we really want you to be using 64 hardware and software.
    dnl if we don't build 64-bit, our test suite fails when comparing the
    dnl results of a 64-bit dev_t from PHP's stat info
    CFLAGS="$CFLAGS -m64"
    ;;
  *darwin*)
    dnl avoid compilation failure on OS X when building with python
    ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    AC_SUBST(ARCHFLAGS)
    ;;
esac

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
                          Default is /etc/watchman.json
],[
  conffile=$enableval
])
if test "$conffile" != "no" ; then
  AC_DEFINE_UNQUOTED(WATCHMAN_CONFIG_FILE, "$conffile",
                     [system configuration file path])
fi

AC_ARG_WITH(buildinfo, [
  --with-buildinfo=TEXT   Include some extra build information that will
                          be reported in the version command output
],[
 AC_DEFINE_UNQUOTED(WATCHMAN_BUILD_INFO, "$withval", [build info])
])


WATCHMAN_STATE_DIR="\$(prefix)/var/run/watchman"
AC_ARG_ENABLE(statedir, [
  --enable-statedir=PATH  Use PATH as the default for state, log files
                          and sockets instead of using your system tempdir
],[
 WATCHMAN_STATE_DIR="$enableval"
])
AC_SUBST(WATCHMAN_STATE_DIR)

want_python=no
AC_ARG_WITH(python, [
  --with-python        Enable python bindings
],[
   want_python="$withval"
])
if test "x$want_python" = "xyes" ; then
  AM_PATH_PYTHON(,,[:])
fi
AM_CONDITIONAL(HAVE_PYTHON, [test "$PYTHON" != : -a "x$want_python" = "xyes"])

want_ruby=no
AC_ARG_WITH(ruby, [
  --with-ruby          Enable ruby bindings (requires ruby, gem, bundler)
],[
   want_ruby="$withval"
])
AM_CONDITIONAL(HAVE_RUBY, [test "x$want_ruby" = "xyes"])

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
  if test "$pcre_config" = "yes" ; then
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
AC_CHECK_FUNCS(mkostemp kqueue port_create inotify_init strtoll localeconv statfs)
AC_CHECK_FUNCS(accept4)
AC_CHECK_HEADERS(sys/vfs.h sys/param.h sys/mount.h sys/statfs.h sys/statvfs.h, [], [],
[[#ifdef __OpenBSD__
# include <sys/param.h>
#endif
]])
AC_CHECK_HEADERS(valgrind/valgrind.h)
AC_CHECK_HEADERS(execinfo.h)
AC_CHECK_HEADERS(CoreServices/CoreServices.h, [
  LIBS="$LIBS -framework CoreServices"
])
AC_CHECK_FUNCS(backtrace backtrace_symbols)

if test -n "$ac_cv_header_sys_statvfs_h"; then
AC_CHECK_MEMBERS([struct statvfs.f_fstypename,struct statvfs.f_basetype],
  [AC_DEFINE([STATVFS_HAS_FSTYPE_AS_STRING], [1],[if statvfs holds fstype as string])],[],[[#include <sys/statvfs.h>]])
fi


# Do this after we've looked for functions
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -Wall -Wextra -Wdeclaration-after-statement -g -gdwarf-2 -fno-omit-frame-pointer"
fi
if test -n "$GCC" -a "$stack_protect" = "yes" ; then
  CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4"
fi
IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" = "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile thirdparty/jansson/jansson_config.h])
AC_OUTPUT

cat << EOF

Your build configuration:

        CPPFLAGS = $CPPFLAGS
        CFLAGS = $CFLAGS
        LDFLAGS = $LDFLAGS
        prefix: $prefix
        version: $VERSION

EOF

dnl vim:ts=2:sw=2:

