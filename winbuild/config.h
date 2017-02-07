/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
// Shim for building on Win32
#pragma once
#define _CRT_SECURE_NO_WARNINGS 1

#ifndef WATCHMAN_SUPER_PEDANTIC
// Each of these is something that we should address :-/

// Buffer overrun; false positives unless we use functions like strcpy_s
#pragma warning(disable: 6386)
// NULL pointer deref
#pragma warning(disable: 6011)

// sign mismatch in printf args
#pragma warning(disable: 6340)

// String might not be zero terminated (false positives)
#pragma warning(disable: 6054)

#endif

#define _ALLOW_KEYWORD_MACROS
#ifndef __cplusplus
#define inline __inline
#endif

// Tell windows.h not to #define min/max
#define NOMINMAX

#define WIN32_LEAN_AND_MEAN
#define EX_USAGE 1
#include <errno.h>
#include <io.h>
#include <process.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <windows.h>

#if _MSC_VER >= 1400
# include <sal.h>
# if _MSC_VER > 1400
#  define WATCHMAN_FMT_STRING(x) _Printf_format_string_ x
# else
#  define WATCHMAN_FMT_STRING(x) __format_string x
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef ptrdiff_t ssize_t;

const char *win32_strerror(DWORD err);
int map_win32_err(DWORD err);
int map_winsock_err(void);

#define snprintf _snprintf
int asprintf(char **out, WATCHMAN_FMT_STRING(const char *fmt), ...);
int vasprintf(char **out, WATCHMAN_FMT_STRING(const char *fmt), va_list ap);
char *dirname(char *path);

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int gethostname(char *buf, int size);
char *realpath(const char *filename, char *target);

#define O_DIRECTORY _O_OBTAIN_DIR
#define O_CLOEXEC _O_NOINHERIT
#define O_NOFOLLOW 0 /* clowny, but there's no translation */

typedef DWORD pid_t;

#define HAVE_BACKTRACE
#define HAVE_BACKTRACE_SYMBOLS
size_t backtrace(void **frames, size_t n_frames);
char **backtrace_symbols(void **array, size_t n_frames);
size_t backtrace_from_exception(
    LPEXCEPTION_POINTERS exception,
    void** frames,
    size_t n_frames);

#ifdef __cplusplus
}
#endif

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <locale.h> header file. */
#define HAVE_LOCALE_H 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the `mkostemp' function. */
/* #undef HAVE_MKOSTEMP */

/* Define to 1 if you have the <pcre.h> header file. */
//#define HAVE_PCRE_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the `strtoll' function. */
#define HAVE_STRTOLL 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
//#define HAVE_UNISTD_H 1

/* Define to 1 if you have the <valgrind/valgrind.h> header file. */
/* #undef HAVE_VALGRIND_VALGRIND_H */

/* Name of package */
#define PACKAGE "watchman"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "watchman"

/* Define to the version of this package. */
#define PACKAGE_VERSION "4.9.0"

/* Version number of package */
#define VERSION PACKAGE_VERSION

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "watchman " PACKAGE_VERSION

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "watchman"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* if statvfs holds fstype as string */
/* #undef STATVFS_HAS_FSTYPE_AS_STRING */

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Use gimli */
/* #undef USE_GIMLI */

/* build info */
/* #undef WATCHMAN_BUILD_INFO */

/* system configuration file path */
#define WATCHMAN_CONFIG_FILE "/etc/watchman.json"

/* default location for state */
/* #undef WATCHMAN_STATE_DIR */

/* Define WORDS_BIGENDIAN to 1 if your processor stores words with the most
   significant byte first (like Motorola and SPARC, unlike Intel). */
#if defined AC_APPLE_UNIVERSAL_BUILD
# if defined __BIG_ENDIAN__
#  define WORDS_BIGENDIAN 1
# endif
#else
# ifndef WORDS_BIGENDIAN
/* #  undef WORDS_BIGENDIAN */
# endif
#endif
