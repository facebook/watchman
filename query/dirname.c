/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct dirname_data {
  w_string_t *dirname;
  struct w_query_int_compare depth;
  bool (*startswith)(w_string_t *str, w_string_t *prefix);
};

static void dispose_dirname(void *ptr) {
  struct dirname_data *data = ptr;

  if (data->dirname) {
    w_string_delref(data->dirname);
  }
  free(data);
}

static bool eval_dirname(struct w_query_ctx *ctx,
    struct watchman_file *file, void *ptr) {
  struct dirname_data *data = ptr;
  w_string_t *str = w_query_ctx_get_wholename(ctx);
  json_int_t depth = 0;
  size_t i;

  unused_parameter(file);

  if (str->len <= data->dirname->len) {
    // Either it doesn't prefix match, or file name is == dirname.
    // That means that the best case is that the wholename matches.
    // we only want to match if dirname(wholename) matches, so it
    // is not possible for us to match unless the length of wholename
    // is greater than the dirname operand
    return false;
  }

  // Want to make sure that wholename is a child of dirname, so
  // check for a dir separator.  Special case for dirname == '' (the root),
  // which won't have a slash in position 0.
  if (data->dirname->len > 0 && str->buf[data->dirname->len] != '/') {
    // may have a common prefix with, but is not a child of dirname
    return false;
  }

  if (!data->startswith(str, data->dirname)) {
    return false;
  }

  // Now compute the depth of file from dirname.  We do this by
  // counting dir separators, not including the one we saw above.
  for (i = data->dirname->len + 1; i < str->len; i++) {
    if (str->buf[i] == '/') {
      depth++;
    }
  }

  return eval_int_compare(depth, &data->depth);
}

// ["dirname", "foo"] -> ["dirname", "foo", ["depth", "ge", 0]]
static w_query_expr *dirname_parser_inner(w_query *query, json_t *term,
    bool caseless)
{
  const char *which = caseless ? "idirname" : "dirname";
  struct dirname_data *data;
  json_t *name;
  struct w_query_int_compare depth_comp;

  if (!json_is_array(term)) {
    ignore_result(asprintf(&query->errmsg, "Expected array for '%s' term",
        which));
    return NULL;
  }

  if (json_array_size(term) < 2) {
    ignore_result(asprintf(&query->errmsg,
        "Invalid number of arguments for '%s' term",
        which));
    return NULL;
  }

  if (json_array_size(term) > 3) {
    ignore_result(asprintf(&query->errmsg,
        "Invalid number of arguments for '%s' term",
        which));
    return NULL;
  }

  name = json_array_get(term, 1);
  if (!json_is_string(name)) {
    ignore_result(asprintf(&query->errmsg,
        "Argument 2 to '%s' must be a string", which));
    return NULL;
  }

  if (json_array_size(term) == 3) {
    json_t *depth;

    depth = json_array_get(term, 2);
    if (!json_is_array(depth)) {
      ignore_result(asprintf(&query->errmsg,
        "Invalid number of arguments for '%s' term",
        which));
      return NULL;
    }

    if (!parse_int_compare(depth, &depth_comp, &query->errmsg)) {
      return NULL;
    }

    if (strcmp("depth", json_string_value(json_array_get(depth, 0)))) {
      ignore_result(asprintf(&query->errmsg,
            "Third parameter to '%s' should be a relational depth term",
            which));
      return NULL;
    }
  } else {
    depth_comp.operand = 0;
    depth_comp.op = W_QUERY_ICMP_GE;
  }

  data = calloc(1, sizeof(*data));
  if (!data) {
    ignore_result(asprintf(&query->errmsg, "out of memory"));
    return NULL;
  }
  data->dirname = w_string_new(json_string_value(name));
  data->startswith =
    caseless ?  w_string_startswith_caseless : w_string_startswith;
  data->depth = depth_comp;

  return w_query_expr_new(eval_dirname, dispose_dirname, data);
}

static w_query_expr *dirname_parser(w_query *query, json_t *term)
{
  return dirname_parser_inner(query, term, !query->case_sensitive);
}
W_TERM_PARSER("dirname", dirname_parser)

static w_query_expr *idirname_parser(w_query *query, json_t *term)
{
  return dirname_parser_inner(query, term, true);
}
W_TERM_PARSER("idirname", idirname_parser)

/* vim:ts=2:sw=2:et:
 */
