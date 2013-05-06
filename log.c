/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

int log_level = W_LOG_ERR;

void w_log(int level, const char *fmt, ...)
{
  char buf[4096];
  va_list ap;
  int len;
  uint32_t tid = (uint32_t)(uintptr_t)pthread_self();

  len = snprintf(buf, sizeof(buf),
        "%d: tid=%" PRIu32 " ", (int)time(NULL), tid);
  va_start(ap, fmt);
  len += vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);

  if (level <= log_level) {
    ignore_result(write(STDERR_FILENO, buf, len));
  }

  w_log_to_clients(level, buf);
}

/* vim:ts=2:sw=2:et:
 */

