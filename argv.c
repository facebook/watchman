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

/* Duplicates an argv array, constructing an argv array that
 * occupies a single contiguous chunk of memory such that all
 * data associated with it can be released by a single call
 * to free(3) the returned array.
 * The last element of the returned argv is set to NULL
 * for compatibility with posix_spawn() */
char **w_argv_dup(int argc, char **argv)
{
  int len = (1 + argc) * sizeof(char*);
  int i;
  char **dup_argv;
  char *buf;

  for (i = 0; i < argc; i++) {
    len += strlen(argv[i]) + 1;
  }

  dup_argv = malloc(len);
  if (!dup_argv) {
    return NULL;
  }

  buf = (char*)(dup_argv + argc + 1);

  for (i = 0; i < argc; i++) {
    dup_argv[i] = buf;
    len = strlen(argv[i]);
    memcpy(buf, argv[i], len);
    buf[len] = 0;
    buf += len + 1;
  }

  dup_argv[argc] = NULL;

  return dup_argv;
}

/* parses a string into a space separate argv array, respecting
 * single and double quoted tokens and backslashed escapes.
 * Caller must free(3) argv_ptr and this will release all
 * memory retained by the argv array.
 * The last element of the returned argv is set to NULL
 * for compatibility with posix_spawn() */
bool w_argv_parse(const char *text, int *argc_ptr, char ***argv_ptr)
{
  int argc, len, i;
  char *pos, *end;
  int quoted;
  char *dup_line;
  char **argv;

  /* first make a pass to approximate how many args we might want
   * to fit in our vector; we over estimate in the face of quoting */
  len = strlen(text);
  argc = 1;
  for (i = 0; i < len; i++) {
    if (text[i] == ' ' || text[i] == '\t') {
      argc++;
    }
  }

  /* arrange the vector at the front of the memory we return
   * and put the string data after it, so that the caller only
   * needs to free(argv) to get everything */
  argv = malloc(len + 1 + ((1 + argc) * sizeof(char*)));
  if (!argv) {
    return false;
  }

  dup_line = (char*)(argv + argc + 1);
  memcpy(dup_line, text, len + 1);

  /* parse into an argument vector */
  end = dup_line + len;
  pos = dup_line;
  argc = 0;
  while (pos < end) {
    // Skip space
    if (*pos == ' ' || *pos == '\t') {
      pos++;
      continue;
    }

    // Figure out quoting
    if (*pos == '"' || *pos == '\'') {
      quoted = *pos;
      pos++;
    } else {
      quoted = 0;
    }

    // Collect this word
    argv[argc++] = pos;
    for (;;) {
      if (*pos == 0) {
        // End of the input string
        if (quoted) {
          goto error;
        }
        // We'll also break out of the while because
        // pos >= end
        assert(pos >= end);
        break;
      }

      if (quoted && *pos == '\\') {
        // de-quote by sliding the memory down
        memmove(pos, pos + 1, end - (pos + 1));
        // this made the string shorter
        end--;
        // we ate the quote and put the quoted character
        // in its place; now we need to advance to the next
        // character
        pos++;
        continue;
      }

      if ((quoted && *pos == quoted) ||
          (!quoted && (*pos == ' ' || *pos == '\t'))) {
        // End of word; terminate it
        *pos = 0;
        // and advance past
        pos++;
        // Ready for next word
        break;
      }

      // Accumulate
      pos++;
    }
  }

  *argc_ptr = argc;
  *argv_ptr = argv;
  argv[argc] = NULL;

  return true;

error:
  free(argv);
  return false;
}

/* vim:ts=2:sw=2:et:
 */
