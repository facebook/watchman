/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"

/* Constructs an argv array by copying elements from a json
 * array.  The returned argv array occupies a single contiguous
 * block of memory such that it can be release by a single call
 * to free(3).
 * The last element of the returned argv is set to NULL for
 * compatibility with posix_spawn() */
char **w_argv_copy_from_json(json_t *arr, int skip)
{
  int argc = json_array_size(arr) - skip;
  int len = (1 + argc) * sizeof(char*);
  uint32_t i;
  char **dup_argv;
  char *buf;
  const char *str;

  /* compute required size, and sanity check the
   * element types */
  for (i = skip; i < json_array_size(arr); i++) {
    json_t *ele = json_array_get(arr, i);
    if (!ele) {
      return NULL;
    }
    str = json_string_value(ele);
    if (!str) {
      return NULL;
    }
    len += strlen(str) + 1;
  }

  dup_argv = malloc(len);
  if (!dup_argv) {
    return NULL;
  }

  buf = (char*)(dup_argv + argc + 1);

  for (i = skip; i < json_array_size(arr); i++) {
    str = json_string_value(json_array_get(arr, i));

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
