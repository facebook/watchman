/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_SYSTEM_H
#define WATCHMAN_SYSTEM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define __STDC_LIMIT_MACROS
#define __STDC_FORMAT_MACROS
#include "config.h"

#ifdef WATCHMAN_FACEBOOK_INTERNAL
#include "common/base/BuildInfo.h"
#undef PACKAGE_VERSION
#define PACKAGE_VERSION BuildInfo_kTimeISO8601
#define WATCHMAN_BUILD_INFO BuildInfo_kUpstreamRevision
#endif

#include <assert.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>
#if HAVE_SYS_INOTIFY_H
# include <sys/inotify.h>
#endif
#if HAVE_SYS_EVENT_H
# include <sys/event.h>
#endif
#if HAVE_PORT_H
# include <port.h>
#endif
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#ifndef _WIN32
#include <grp.h>
#include <libgen.h>
#endif
#include <inttypes.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <fcntl.h>
#if defined(__linux__) && !defined(O_CLOEXEC)
# define O_CLOEXEC   02000000 /* set close_on_exec, from asm/fcntl.h */
#endif
#ifndef O_CLOEXEC
# define O_CLOEXEC 0
#endif
#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#endif
#ifdef HAVE_PCRE_H
# include <pcre.h>
#endif
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif
#ifndef _WIN32
#include <sys/uio.h>
#include <pwd.h>
#include <sysexits.h>
#endif
#include <spawn.h>
#include <stddef.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif

#ifdef _WIN32
# define PRIsize_t "Iu"
#else
# define PRIsize_t "zu"
#endif

#if defined(__clang__)
# if __has_feature(address_sanitizer)
#  define WATCHMAN_ASAN 1
# endif
#elif defined (__GNUC__) && \
      (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ >= 5)) && \
      __SANITIZE_ADDRESS__
# define WATCHMAN_ASAN 1
#endif

#ifndef WATCHMAN_ASAN
# define WATCHMAN_ASAN 0
#endif

#ifdef HAVE_CORESERVICES_CORESERVICES_H
# include <CoreServices/CoreServices.h>
# if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
#  define HAVE_FSEVENTS 0
# else
#  define HAVE_FSEVENTS 1
# endif
#endif

// We make use of constructors to glue together modules
// without maintaining static lists of things in the build
// configuration.  These are helpers to make this work
// more portably
#ifdef _WIN32
#pragma section(".CRT$XCU", read)
# define w_ctor_fn_type(sym) void __cdecl sym(void)
# define w_ctor_fn_reg(sym) \
  static __declspec(allocate(".CRT$XCU")) \
    void (__cdecl * w_paste1(sym, _reg))(void) = sym;
#else
# define w_ctor_fn_type(sym) \
  __attribute__((constructor)) void sym(void)
# define w_ctor_fn_reg(sym) /* not needed */
#endif

/* sane, reasonably large filename size that we'll use
 * throughout; POSIX seems to define smallish buffers
 * that seem risky */
#define WATCHMAN_NAME_MAX   4096

// rpmbuild may enable fortify which turns on
// warn_unused_result on a number of system functions.
// This gives us a reasonably clean way to suppress
// these warnings when we're using stack protection.
#if __USE_FORTIFY_LEVEL > 0
# define ignore_result(x) \
  do { __typeof__(x) _res = x; (void)_res; } while(0)
#elif _MSC_VER >= 1400
# define ignore_result(x) \
  do { int _res = (int)x; (void)_res; } while(0)
#else
# define ignore_result(x) x
#endif

// self-documenting hint to the compiler that we didn't use it
#define unused_parameter(x)  (void)x

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
// Not explicitly exported on Darwin, so we get to define it.
extern char **environ;
#endif

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
