/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool eval_exists(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  unused_parameter(ctx);
  unused_parameter(data);

  return file->exists;
}

w_query_expr *w_expr_exists_parser(w_query *query, json_t *term)
{
  unused_parameter(query);
  unused_parameter(term);
  return w_query_expr_new(eval_exists, NULL, NULL);
}

static bool eval_empty(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  unused_parameter(ctx);
  unused_parameter(data);

  if (!file->exists) {
    return false;
  }

  if (S_ISDIR(file->st.st_mode) || S_ISREG(file->st.st_mode)) {
    return file->st.st_size == 0;
  }

  return false;
}

w_query_expr *w_expr_empty_parser(w_query *query, json_t *term)
{
  unused_parameter(query);
  unused_parameter(term);
  return w_query_expr_new(eval_empty, NULL, NULL);
}


/* vim:ts=2:sw=2:et:
 */

