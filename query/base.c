/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Basic boolean and compound expressions */

struct w_expr_list {
  bool allof;
  uint32_t num;
  w_query_expr **exprs;
};

static void dispose_expr(void *data)
{
  w_query_expr *expr = data;

  w_query_expr_delref(expr);
}

static bool eval_not(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  w_query_expr *expr = data;

  return !w_query_expr_evaluate(expr, ctx, file);
}

w_query_expr *w_expr_not_parser(w_query *query, json_t *term)
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

static bool eval_bool(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  unused_parameter(ctx);
  unused_parameter(file);
  return data ? true : false;
}

w_query_expr *w_expr_true_parser(w_query *query, json_t *term)
{
  unused_parameter(term);
  unused_parameter(query);
  return w_query_expr_new(eval_bool, NULL, (void*)1);
}

w_query_expr *w_expr_false_parser(w_query *query, json_t *term)
{
  unused_parameter(term);
  unused_parameter(query);
  return w_query_expr_new(eval_bool, NULL, 0);
}

static bool eval_list(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  struct w_expr_list *list = data;
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
  struct w_expr_list *list = data;
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

  list = calloc(1, sizeof(*list));
  if (!list) {
    query->errmsg = strdup("out of memory");
    return NULL;
  }

  list->allof = allof;
  list->num = json_array_size(term) - 1;
  list->exprs = calloc(list->num, sizeof(list->exprs[0]));

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

w_query_expr *w_expr_anyof_parser(w_query *query, json_t *term)
{
  return parse_list(query, term, false);
}

w_query_expr *w_expr_allof_parser(w_query *query, json_t *term)
{
  return parse_list(query, term, true);
}

/* vim:ts=2:sw=2:et:
 */

