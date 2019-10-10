/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

int asprintf(char** out, WATCHMAN_FMT_STRING(const char* fmt), ...) {
  va_list ap;
  int len;

  va_start(ap, fmt);
  len = vasprintf(out, fmt, ap);
  va_end(ap);

  return len;
}
