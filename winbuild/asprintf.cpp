/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Poor-mans asprintf

int vasprintf(char **out, WATCHMAN_FMT_STRING(const char *fmt), va_list ap) {
  char stack[512];
  char *buf = stack;
  char *newbuf;
  int alloc = sizeof(stack);
  int len;

  while (1) {
    // For VC, returns -1 when it truncates
    len = _vsnprintf(buf, alloc, fmt, ap);

    if (len >= 0) {
      // It fits!
      if (buf == stack) {
        *out = strdup(buf);
        return len;
      }
      *out = buf;
      return len;
    }

    // Need a bigger boat
    alloc *= 2;
    newbuf = (char*)realloc(buf == stack ? NULL : buf, alloc);
    if (!newbuf) {
      if (buf != stack) {
        free(buf);
      }
      *out = NULL;
      return -1;
    }
    buf = newbuf;
  }
}

int asprintf(char **out, WATCHMAN_FMT_STRING(const char *fmt), ...) {
  va_list ap;
  int len;

  va_start(ap, fmt);
  len = vasprintf(out, fmt, ap);
  va_end(ap);

  return len;
}
