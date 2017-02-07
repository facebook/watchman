AC_INIT([watchman], [4.9.0], [], [watchman])
AC_CONFIG_MACRO_DIR([m4])
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
    CXXFLAGS="$CXXFLAGS -m64"
    ;;
  *darwin*)
    dnl avoid compilation failure on OS X when building with python
    ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    AC_SUBST(ARCHFLAGS)
    ;;
esac
AC_PROG_CC
AC_PROG_CXX
AX_CXX_COMPILE_STDCXX_11([noext],[mandatory])
AC_PROG_CPP
AC_C_BIGENDIAN
AC_C_INLINE

LT_INIT

AM_PROG_CC_C_O
AM_PROG_AS

AC_ARG_ENABLE([cppclient],
  AS_HELP_STRING([--disable-cppclient], [Disable build of C++ client library]))

AC_ARG_WITH([folly],
  AS_HELP_STRING([--with-folly=<path to folly>], [Where to find folly]))

AS_IF([test "x$with_folly" != "x"], [
  folly_config_dir=$with_folly/lib/pkgconfig/
  AS_IF([test -f "$folly_config_dir/libfolly.pc"],
    [export PKG_CONFIG_PATH=$folly_config_dir:$PKG_CONFIG_PATH],
    [AC_MSG_ERROR([cannot find folly config $folly_config_dir/libfolly.pc])]
	)
])


dnl
dnl Decide whether to build the C++ client library
dnl
PKG_CHECK_MODULES([FOLLY], [libfolly], [folly=yes], [folly=no])
AS_IF([test "x$enable_cppclient" = "xyes" && test "x$folly" = "xno"],
  [AC_MSG_ERROR([cppclient requested but libfolly not present])])
AS_IF([test "x$enable_cppclient" != "xno" && test "x$folly" = "xno"],
  [enable_cppclient=no])
AM_CONDITIONAL(ENABLE_CPPCLIENT, [test "x$enable_cppclient" != "xno"])


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

build_dev_fsevents=no
AC_ARG_WITH(fsevents-header, [
  --with-fsevents-header=/path/to/xnu/bsd/sys/fsevents.h
                          Specify the path to the fsevents.h header file
                          on your system.  This is an optional dependency
                          for the experimental devfsevents watcher.
],[
  fsevents_header="$withval"
  AC_MSG_CHECKING([that $fsevents_header is a file])
  if test -f "$fsevents_header" ; then
    AC_DEFINE_UNQUOTED(SYS_FSEVENTS_PATH, ["$fsevents_header"],[sys/fsevents.h location])
    AC_MSG_RESULT(yes)
    build_dev_fsevents=yes
  else
    AC_MSG_FAILURE([--with-fsevents-header was given but the value $fsevents_header was not a file])
  fi
])
AM_CONDITIONAL(DEV_FSEVENTS, [test "$build_dev_fsevents" != "no"])

xprefix=$prefix
if test "$xprefix" = "NONE"; then
  xprefix=$ac_default_prefix
fi

WATCHMAN_STATE_DIR="$xprefix/var/run/watchman"
AC_ARG_ENABLE(statedir, [
  --enable-statedir=PATH  Use PATH as the default for state, log files
                          and sockets instead of using your system tempdir
],[
 WATCHMAN_STATE_DIR="$enableval"
])

if test "$WATCHMAN_STATE_DIR" != "no"; then
  AC_DEFINE_UNQUOTED(WATCHMAN_STATE_DIR,
    ["$WATCHMAN_STATE_DIR"],
    [watchman state directory])
  AC_SUBST(WATCHMAN_STATE_DIR)
fi
AM_CONDITIONAL(WATCHMAN_STATE_DIR, [test "$WATCHMAN_STATE_DIR" != "no"])

AC_ARG_ENABLE(asan, [
  --enable-asan           Enable address sanitizer options
])
AC_ARG_ENABLE(opt, [
  --disable-opt           Disable optimizations (-O0)
])

want_python=yes
AC_ARG_WITH(python, [
  --with-python=PATH      Enable Python bindings. PATH is location of python.
                          Default is to look for python in your PATH
  --without-python        Disable python bindings
],[
   want_python="$withval"
])
if test "x$want_python" != "xno" ; then
  if test "$want_python" != "yes"; then
    PYTHON="$want_python"
  fi
  AM_PATH_PYTHON(,,[:])
fi
AM_CONDITIONAL(HAVE_PYTHON, [test "$PYTHON" != : -a "x$want_python" != "xno"])

want_ruby=no
AC_ARG_WITH(ruby, [
  --with-ruby          Enable ruby bindings (requires ruby, gem, bundler)
],[
   want_ruby="$withval"
])
AM_CONDITIONAL(HAVE_RUBY, [test "x$want_ruby" = "xyes"])

AC_ARG_WITH(pcre, [
  --without-pcre       Don't enable pcre support.
  --with-pcre=PATH     Enable pcre support.  PATH is location of pcre-config.
                       Default is to enable and look for pcre-config in your
                       $PATH
],[
  pcre_config="$withval"
],[
  pcre_config="check"
])

if test "$pcre_config" != "no" ; then
  if test "$pcre_config" = "yes" -o "$pcre_config" = "check"; then
    AC_PATH_PROG(PCRE_CONFIG_PATH, pcre-config, false)
    dnl If --with-pcre was specified but pcre-config not found, fail hard now.
    if test "$pcre_config" = "yes" -a "$PCRE_CONFIG_PATH" = "false"; then
      AC_MSG_FAILURE([--with-pcre was given, but pcre-config not found in PATH])
    fi
  else
    PCRE_CONFIG_PATH="$pcre_config"
  fi

  AC_MSG_CHECKING(for PCRE using $PCRE_CONFIG_PATH)
  pcre_version=`"$PCRE_CONFIG_PATH" --version 2>/dev/null`
  if test $? -ne 0; then
    AC_MSG_RESULT(failed)
  else
    AC_MSG_RESULT($pcre_version)
  fi

  if test -n "$pcre_version"; then
    PCRE_LIBS=`$PCRE_CONFIG_PATH --libs`
    LIBS="$LIBS $PCRE_LIBS"
    PCRE_CFLAGS=`$PCRE_CONFIG_PATH --cflags`
    CFLAGS="$CFLAGS $PCRE_CFLAGS"
    CPPFLAGS="$CPPFLAGS $PCRE_CFLAGS"
    AC_CHECK_HEADERS(pcre.h, [], [
      if test "$pcre_config" != "check"; then
         AC_MSG_FAILURE([--with-pcre was given, but pcre not found:
  pcre-config --libs=$PCRE_LIBS
  pcre-config --cflags=$PCRE_CFLAGS])
      fi
    ])
  elif test "$pcre_config" != "check"; then
    AC_MSG_FAILURE([$PCRE_CONFIG_PATH failed to run, could not check for PCRE])
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
AC_SEARCH_LIBS([SHA1_Init], [crypto])

AC_CHECK_HEADERS(sys/types.h inttypes.h locale.h port.h sys/inotify.h sys/event.h)
AC_CHECK_HEADERS(sys/ucred.h sys/socket.h)
AC_CHECK_FUNCS(mkostemp kqueue port_create inotify_init strtoll localeconv statfs)
AC_CHECK_FUNCS(accept4 inotify_init1 getattrlistbulk openat fdopendir pipe2)
AC_CHECK_HEADERS(sys/vfs.h sys/param.h sys/mount.h sys/statfs.h sys/statvfs.h, [], [],
[[#ifdef __OpenBSD__
# include <sys/param.h>
#endif
]])
AC_CHECK_HEADERS(valgrind/valgrind.h)
AC_CHECK_HEADERS(execinfo.h sys/resource.h)
AC_CHECK_HEADERS(CoreServices/CoreServices.h, [
  LIBS="$LIBS -framework CoreServices"
])
AC_CHECK_FUNCS(FSEventStreamSetExclusionPaths)
AC_CHECK_FUNCS(backtrace backtrace_symbols backtrace_symbols_fd)
AC_CHECK_FUNCS(sys_siglist)
AC_CHECK_FUNCS(memmem)

if test -n "$ac_cv_header_sys_statvfs_h"; then
AC_CHECK_MEMBERS([struct statvfs.f_fstypename,struct statvfs.f_basetype],
  [AC_DEFINE([STATVFS_HAS_FSTYPE_AS_STRING], [1],[if statvfs holds fstype as string])],[],[[#include <sys/statvfs.h>]])
fi


# Do this after we've looked for functions
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -Wall -Wextra -Wdeclaration-after-statement -g -gdwarf-2 -fno-omit-frame-pointer"
  CXXFLAGS="$CXXFLAGS -Wall -Wextra -g -gdwarf-2 -fno-omit-frame-pointer"
fi
if test -n "$GCC" -a "x$enable_opt" = "xno" ; then
  CFLAGS="$CFLAGS -O0"
  CXXFLAGS="$CXXFLAGS -O0"
  LDFLAGS="$LDFLAGS -O0"
fi
if test -n "$GCC" -a "x$enable_asan" = "xyes" ; then
  CFLAGS="$CFLAGS -fsanitize=address"
  CXXFLAGS="$CXXFLAGS -fsanitize=address"
  LDFLAGS="$LDFLAGS -fsanitize=address"
fi
if test -n "$GCC" -a "$stack_protect" = "yes" ; then
  CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4"
  CXXFLAGS="$CXXFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4"
fi
IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" = "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([
  Makefile
  thirdparty/jansson/jansson_config.h
  cppclient/watchmanclient.pc
])
AC_OUTPUT

if test "$WATCHMAN_STATE_DIR" != "no"; then
  reported_state_directory="$WATCHMAN_STATE_DIR"
else
  reported_state_directory="System temporary directory"
fi

cat << EOF

Your build configuration:

        CC = $CC
        CPPFLAGS = $CPPFLAGS
        CFLAGS = $CFLAGS
        CXX = $CXX
        CXXFLAGS = $CXXFLAGS
        LDFLAGS = $LDFLAGS
        prefix: $prefix
        version: $VERSION
        state directory: $reported_state_directory

EOF

dnl vim:ts=2:sw=2:
