/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static w_ht_t *term_hash = NULL;

bool w_query_register_expression_parser(
    const char *term,
    w_query_expr_parser parser)
{
  w_string_t *name = w_string_new(term);

  if (!name) {
    return false;
  }

  if (!term_hash) {
    term_hash = w_ht_new(32, &w_ht_string_funcs);
  }

  return w_ht_set(term_hash, w_ht_ptr_val(name), w_ht_ptr_val(parser));
}

/* parse an expression term. It can be one of:
 * "term"
 * ["term" <parameters>]
 */
w_query_expr *w_query_expr_parse(w_query *query, json_t *exp)
{
  w_string_t *name;
  w_query_expr_parser parser;

  if (json_is_string(exp)) {
    name = w_string_new(json_string_value(exp));
  } else if (json_is_array(exp) && json_array_size(exp) > 0) {
    json_t *first = json_array_get(exp, 0);

    if (!json_is_string(first)) {
      query->errmsg = strdup(
          "first element of an expression must be a string");
      return NULL;
    }
    name = w_string_new(json_string_value(first));
  } else {
    query->errmsg = strdup("expected array or string for an expression");
    return NULL;
  }

  parser = w_ht_val_ptr(w_ht_get(term_hash, w_ht_ptr_val(name)));

  if (!parser) {
    ignore_result(asprintf(&query->errmsg,
        "unknown expression term '%s'",
        name->buf));
    w_string_delref(name);
    return NULL;
  }
  w_string_delref(name);
  return parser(query, exp);
}

static bool parse_since(w_query *res, json_t *query)
{
  json_t *since;

  since = json_object_get(query, "since");
  if (!since) {
    return true;
  }

  if (json_is_integer(since) || json_is_string(since)) {
    /* copy the reference; we'll evaluate it at execution time */
    res->since = since;
    json_incref(since);
    return true;
  }

  res->errmsg = strdup("invalid value for 'since'");

  return false;
}

static bool parse_suffixes(w_query *res, json_t *query)
{
  json_t *suffixes;
  size_t i;

  suffixes = json_object_get(query, "suffix");
  if (!suffixes) {
    return true;
  }

  if (!json_is_array(suffixes)) {
    res->errmsg = strdup("'suffix' must be an array of strings");
    return false;
  }

  res->nsuffixes = json_array_size(suffixes);
  res->suffixes = calloc(res->nsuffixes, sizeof(w_string_t*));

  if (!res->suffixes) {
    return false;
  }

  for (i = 0; i < json_array_size(suffixes); i++) {
    json_t *ele = json_array_get(suffixes, i);
    char *low;
    int j;

    if (!json_is_string(ele)) {
      res->errmsg = strdup("'suffix' must be an array of strings");
      return false;
    }

    low = strdup(json_string_value(ele));
    if (!low) {
      res->errmsg = strdup("out of memory");
      return false;
    }

    for (j = 0; low[j]; j++) {
      low[j] = tolower(low[j]);
    }

    res->suffixes[i] = w_string_new(low);

    free(low);
  }

  return true;
}

static bool parse_paths(w_query *res, json_t *query)
{
  json_t *paths;
  size_t i;

  paths = json_object_get(query, "path");
  if (!paths) {
    return true;
  }

  if (!json_is_array(paths)) {
    res->errmsg = strdup("'path' must be an array");
    return false;
  }

  res->npaths = json_array_size(paths);
  res->paths = calloc(res->npaths, sizeof(res->paths[0]));

  if (!res->paths) {
    res->errmsg = strdup("out of memory");
    return false;
  }

  for (i = 0; i < json_array_size(paths); i++) {
    json_t *ele = json_array_get(paths, i);
    const char *name = NULL;
    w_string_t *path;

    res->paths[i].depth = -1;

    if (json_is_string(ele)) {
      name = json_string_value(ele);
    } else if (json_unpack(ele, "{s:s, s:i}",
          "path", &name,
          "depth", &res->paths[i].depth
          ) != 0) {
      res->errmsg = strdup(
          "expected object with 'path' and 'depth' properties"
          );
      return false;
    }

    path = w_string_new(name);
    res->paths[i].name = w_string_canon_path(path);
    w_string_delref(path);
  }

  return true;
}

static bool parse_query_expression(w_query *res, json_t *query)
{
  json_t *exp;

  exp = json_object_get(query, "expression");
  if (!exp) {
    // Empty expression means that we emit all generated files
    return true;
  }

  res->expr = w_query_expr_parse(res, exp);
  if (!res->expr) {
    return false;
  }

  return true;
}

static bool parse_sync(w_query *res, json_t *query)
{
  json_t *timeout;
  int value;

  timeout = json_object_get(query, "sync_timeout");
  if (!timeout) {
    res->sync_timeout = 60000;
    return true;
  }

  if (json_unpack(timeout, "i", &value) != 0 || value < 0) {
    res->errmsg = strdup("sync_timeout must be an integer value >= 0");
    return false;
  }

  res->sync_timeout = value;
  return true;
}

w_query *w_query_parse(json_t *query, char **errmsg)
{
  w_query *res;

  *errmsg = NULL;

  res = calloc(1, sizeof(*res));
  if (!res) {
    *errmsg = strdup("out of memory");
    goto error;
  }
  res->refcnt = 1;

  if (!parse_sync(res, query)) {
    goto error;
  }

  /* Look for path generators */
  if (!parse_paths(res, query)) {
    goto error;
  }

  /* Look for suffix generators */
  if (!parse_suffixes(res, query)) {
    goto error;
  }

  /* Look for since generator */
  if (!parse_since(res, query)) {
    goto error;
  }

  if (!parse_query_expression(res, query)) {
    goto error;
  }

  if (res->npaths + res->nsuffixes == 0 && !res->since) {
    // No generators specified, so we'll visist all files
    res->all_files = true;
  }

  return res;
error:
  if (res) {
    *errmsg = res->errmsg;
    res->errmsg = NULL;
    w_query_delref(res);
  }
  if (!*errmsg) {
    *errmsg = strdup("unspecified error");
  }

  return NULL;
}

bool w_query_legacy_field_list(struct w_query_field_list *flist)
{
  static const char *names[] = {
    "name", "exists", "size", "mode", "uid", "gid", "atime",
    "mtime", "ctime", "ino", "dev", "nlink", "new", "cclock",
    "oclock"
  };
  uint8_t i;
  json_t *list = json_array();
  bool res;
  char *errmsg = NULL;

  for (i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
    json_array_append_new(list, json_string_nocheck(names[i]));
  }

  res = parse_field_list(list, flist, &errmsg);

  json_decref(list);

  if (errmsg) {
    w_log(W_LOG_FATAL, "should never happen: %s\n", errmsg);
  }

  return res;
}

// Translate from the legacy array into the new style, then
// delegate to the main parser.
// We build a big anyof expression
w_query *w_query_parse_legacy(json_t *args, char **errmsg,
    int start, uint32_t *next_arg,
    const char *clockspec)
{
  bool include = true;
  bool negated = false;
  uint32_t i;
  const char *term_name = "match";
  json_t *query_array;
  json_t *included = NULL, *excluded = NULL;
  json_t *term;
  json_t *container;
  json_t *query_obj = json_object();
  w_query *query;

  if (!json_is_array(args)) {
    *errmsg = strdup("Expected an array");
    return NULL;
  }

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!arg) {
      /* not a string value! */
      ignore_result(asprintf(errmsg,
          "rule @ position %d is not a string value", i));
      return NULL;
    }
  }

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }
    if (!strcmp(arg, "-P")) {
      term_name = "ipcre";
      continue;
    }
    if (!strcmp(arg, "-p")) {
      term_name = "pcre";
      continue;
    }

    // Which group are we going to file it into
    if (include) {
      if (!included) {
        included = json_pack("[s]", "anyof");
      }
      container = included;
    } else {
      if (!excluded) {
        excluded = json_pack("[s]", "anyof");
      }
      container = excluded;
    }

    term = json_pack("[sss]", term_name, arg, "wholename");
    if (negated) {
      term = json_pack("[so]", "not", term);
    }
    json_array_append_new(container, term);

    // Reset negated flag
    negated = false;
    term_name = "match";
  }

  if (excluded) {
    term = json_pack("[so]", "not", excluded);
    excluded = term;
  }

  if (included && excluded) {
    query_array = json_pack("[soo]", "allof", excluded, included);
  } else if (included) {
    query_array = included;
  } else {
    query_array = excluded;
  }

  // query_array may be NULL, which means find me all files.
  // Otherwise, it is the expression we want to use.
  if (query_array) {
    json_object_set_new_nocheck(query_obj, "expression", query_array);
  }

  // For trigger
  if (next_arg) {
    *next_arg = i;
  }

  if (clockspec) {
    json_object_set_new_nocheck(query_obj,
        "since", json_string_nocheck(clockspec));
  }

  /* compose the query with the field list */
  query = w_query_parse(query_obj, errmsg);

  json_decref(query_obj);

  return query;
}

void w_query_delref(w_query *query)
{
  uint32_t i;

  if (!w_refcnt_del(&query->refcnt)) {
    return;
  }

  for (i = 0; i < query->npaths; i++) {
    if (query->paths[i].name) {
      w_string_delref(query->paths[i].name);
    }
  }
  free(query->paths);

  if (query->since) {
    json_decref(query->since);
  }

  if (query->expr) {
    w_query_expr_delref(query->expr);
  }

  free(query);
}

w_query_expr *w_query_expr_new(
    w_query_expr_eval_func evaluate,
    w_query_expr_dispose_func dispose,
    void *data
)
{
  w_query_expr *expr;

  expr = calloc(1, sizeof(*expr));
  if (!expr) {
    return NULL;
  }
  expr->refcnt = 1;
  expr->evaluate = evaluate;
  expr->dispose = dispose;
  expr->data = data;

  return expr;
}

void w_query_expr_delref(w_query_expr *expr)
{
  if (!w_refcnt_del(&expr->refcnt)) {
    return;
  }
  if (expr->dispose) {
    expr->dispose(expr->data);
  }
  free(expr);
}


/* vim:ts=2:sw=2:et:
 */

