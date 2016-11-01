/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Constructs an argv array by copying elements from a json
 * array.  The returned argv array occupies a single contiguous
 * block of memory such that it can be released by a single call
 * to free(3).
 * The last element of the returned argv is set to NULL for
 * compatibility with posix_spawn() */
char** w_argv_copy_from_json(const json_ref& arr, int skip) {
  size_t argc = json_array_size(arr) - skip;
  size_t len = (1 + argc) * sizeof(char*);
  uint32_t i;
  char *buf;
  const char *str;

  /* compute required size, and sanity check the
   * element types */
  for (i = skip; i < json_array_size(arr); i++) {
    const auto& ele = arr.at(i);
    str = json_string_value(ele);
    if (!str) {
      return NULL;
    }
    len += strlen(str) + 1;
  }

  auto dup_argv = (char**)malloc(len);
  if (!dup_argv) {
    return NULL;
  }

  buf = (char*)(dup_argv + argc + 1);

  for (i = skip; i < json_array_size(arr); i++) {
    str = json_string_value(arr.at(i));

    dup_argv[i - skip] = buf;
    len = strlen(str);
    memcpy(buf, str, len);
    buf[len] = 0;
    buf += len + 1;
  }

  dup_argv[argc] = NULL;

  return dup_argv;
}

/* vim:ts=2:sw=2:et:
 */
