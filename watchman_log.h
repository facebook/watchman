/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_LOG_H
#define WATCHMAN_LOG_H

// Helpers for pasting __LINE__ for symbol generation
#define w_paste2(pre, post)  pre ## post
#define w_paste1(pre, post)  w_paste2(pre, post)
#define w_gen_symbol(pre)    w_paste1(pre, __LINE__)

#define W_LOG_OFF 0
#define W_LOG_ERR 1
#define W_LOG_DBG 2
#define W_LOG_FATAL -1

#ifndef WATCHMAN_FMT_STRING
# define WATCHMAN_FMT_STRING(x) x
#endif

extern int log_level;
extern char *log_name;
const char *w_set_thread_name(const char *fmt, ...);
const char *w_get_thread_name(void);
void w_setup_signal_handlers(void);
void w_log(int level, WATCHMAN_FMT_STRING(const char *fmt), ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
;

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

bool w_should_log_to_clients(int level);
void w_log_to_clients(int level, const char *buf);

#endif
