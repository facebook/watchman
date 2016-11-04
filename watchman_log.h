/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// You are encouraged to include "Logging.h" and use those functions rather than
// use these.  The functions in this file are best suited to low level or early
// bootstrapping situations.

#ifndef WATCHMAN_LOG_H
#define WATCHMAN_LOG_H

// Coupled with enum LogLevel in Logging.h
#define W_LOG_OFF 0
#define W_LOG_ERR 1
#define W_LOG_DBG 2
#define W_LOG_FATAL -1

#include "watchman_preprocessor.h"

extern int log_level;
extern char *log_name;
const char *w_set_thread_name(const char *fmt, ...);
void w_setup_signal_handlers(void);
void w_log(int level, WATCHMAN_FMT_STRING(const char* fmt), ...)
    WATCHMAN_FMT_ATTR(2, 3);

#define w_check(e, ...)                                                        \
  if (!(e)) {                                                                  \
    w_log(W_LOG_ERR, "%s:%u failed assertion `%s'\n", __FILE__, __LINE__, #e); \
    w_log(W_LOG_FATAL, __VA_ARGS__);                                           \
  }

// Similar to assert(), but uses W_LOG_FATAL to log the stack trace
// before giving up the ghost
#ifdef NDEBUG
# define w_assert(e, ...) ((void)0)
#else
#define w_assert(e, ...) w_check(e, __VA_ARGS__)
#endif

#endif
