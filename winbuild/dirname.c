/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

char *dirname(char *path) {
  char *end;
  uint32_t len = strlen_uint32(path);

  if (len == 0) {
    return path;
  }
  end = path + len - 1;

  while (end > path && *end != '/' && *end != '\\') {
    --end;
  }

  if (*end == '/' || *end == '\\') {
    *end = 0;
  }

  return path;
}

/* vim:ts=2:sw=2:et:
 */
