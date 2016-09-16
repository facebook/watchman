/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Basic boolean and compound expressions */

struct w_expr_list {
  bool allof;
  size_t num;
  w_query_expr **exprs;
};

static void dispose_expr(void *data)
{
  auto expr = (w_query_expr*)data;

  w_query_expr_delref(expr);
}

static bool eval_not(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  auto expr = (w_query_expr*)data;

  return !w_query_expr_evaluate(expr, ctx, file);
}

static w_query_expr *not_parser(w_query *query, json_t *term)
{
  json_t *other;
  w_query_expr *other_expr;

  /* rigidly require ["not", expr] */
  if (!json_is_array(term) || json_array_size(term) != 2) {
    query->errmsg = strdup("must use [\"not\", expr]");
    return NULL;
  }

  other = json_array_get(term, 1);
  other_expr = w_query_expr_parse(query, other);
  if (!other_expr) {
    // other expr sets errmsg
    return NULL;
  }

  return w_query_expr_new(eval_not, dispose_expr, other_expr);
}
W_TERM_PARSER("not", not_parser)

static bool eval_bool(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  unused_parameter(ctx);
  unused_parameter(file);
  return data ? true : false;
}

static w_query_expr *true_parser(w_query *query, json_t *term)
{
  unused_parameter(term);
  unused_parameter(query);
  return w_query_expr_new(eval_bool, NULL, (void*)1);
}
W_TERM_PARSER("true", true_parser)

static w_query_expr *false_parser(w_query *query, json_t *term)
{
  unused_parameter(term);
  unused_parameter(query);
  return w_query_expr_new(eval_bool, NULL, 0);
}
W_TERM_PARSER("false", false_parser)

static bool eval_list(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  auto list = (w_expr_list*)data;
  size_t i;

  for (i = 0; i < list->num; i++) {
    bool res = w_query_expr_evaluate(
        list->exprs[i],
        ctx, file);

    if (!res && list->allof) {
      return false;
    }

    if (res && !list->allof) {
      return true;
    }
  }
  return list->allof;
}

static void dispose_list(void *data)
{
  auto list = (w_expr_list*)data;
  size_t i;

  for (i = 0; i < list->num; i++) {
    if (list->exprs[i]) {
      w_query_expr_delref(list->exprs[i]);
    }
  }

  free(list->exprs);
  free(list);
}

static w_query_expr *parse_list(w_query *query, json_t *term, bool allof)
{
  struct w_expr_list *list;
  size_t i;

  /* don't allow "allof" on its own */
  if (!json_is_array(term) || json_array_size(term) < 2) {
    query->errmsg = strdup("must use [\"allof\", expr...]");
    return NULL;
  }

  list = (w_expr_list*)calloc(1, sizeof(*list));
  if (!list) {
    query->errmsg = strdup("out of memory");
    return NULL;
  }

  list->allof = allof;
  list->num = json_array_size(term) - 1;
  list->exprs = (w_query_expr**)calloc(list->num, sizeof(list->exprs[0]));

  for (i = 0; i < list->num; i++) {
    w_query_expr *parsed;
    json_t *exp = json_array_get(term, i + 1);

    parsed = w_query_expr_parse(query, exp);
    if (!parsed) {
      // other expression parser sets errmsg
      dispose_list(list);
      return NULL;
    }

    list->exprs[i] = parsed;
  }

  return w_query_expr_new(eval_list, dispose_list, list);
}

static w_query_expr *anyof_parser(w_query *query, json_t *term)
{
  return parse_list(query, term, false);
}
W_TERM_PARSER("anyof", anyof_parser)

static w_query_expr *allof_parser(w_query *query, json_t *term)
{
  return parse_list(query, term, true);
}
W_TERM_PARSER("allof", allof_parser)

/* vim:ts=2:sw=2:et:
 */
