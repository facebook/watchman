/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool eval_suffix(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  w_string_t *suffix = data;

  unused_parameter(ctx);

  return w_string_suffix_match(file->name, suffix);
}

static void dispose_suffix(void *data)
{
  w_string_t *suffix = data;

  w_string_delref(suffix);
}

static w_query_expr *suffix_parser(w_query *query, json_t *term)
{
  const char *ignore, *suffix;
  char *arg;
  w_string_t *str;
  int i, l;

  if (json_unpack(term, "[s,s]", &ignore, &suffix) != 0) {
    query->errmsg = strdup("must use [\"suffix\", \"suffixstring\"]");
    return NULL;
  }

  arg = strdup(suffix);
  if (!arg) {
    query->errmsg = strdup("out of memory");
    return NULL;
  }

  l = strlen_uint32(arg);
  for (i = 0; i < l; i++) {
    arg[i] = (char)tolower((uint8_t)arg[i]);
  }

  str = w_string_new(arg);
  free(arg);
  if (!str) {
    query->errmsg = strdup("out of memory");
    return NULL;
  }

  return w_query_expr_new(eval_suffix, dispose_suffix, str);
}
W_TERM_PARSER("suffix", suffix_parser)

/* vim:ts=2:sw=2:et:
 */
