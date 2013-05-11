/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void log_stack_trace(void)
{
#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  void *array[24];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, sizeof(array)/sizeof(array[0]));
  strings = backtrace_symbols(array, size);
  w_log(W_LOG_ERR, "Fatal error detected at:\n");

  for (i = 0; i < size; i++) {
    w_log(W_LOG_ERR, "%s\n", strings[i]);
  }

  free(strings);
#endif
}

int log_level = W_LOG_ERR;

void w_log(int level, const char *fmt, ...)
{
  char buf[4096];
  va_list ap;
  int len;
  uint32_t tid = (uint32_t)(uintptr_t)pthread_self();
  bool fatal = false;

  if (level == W_LOG_FATAL) {
    level = W_LOG_ERR;
    fatal = true;
  }

  len = snprintf(buf, sizeof(buf),
        "%d: tid=%" PRIu32 " ", (int)time(NULL), tid);
  va_start(ap, fmt);
  len += vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);

  if (level <= log_level) {
    ignore_result(write(STDERR_FILENO, buf, len));
  }

  w_log_to_clients(level, buf);

  if (fatal) {
    log_stack_trace();
    abort();
  }
}

/* vim:ts=2:sw=2:et:
 */

