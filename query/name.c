/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct name_data {
  w_string_t *name;
  // TODO: allow map syntax to work
  w_ht_t *map;
  bool caseless;
  bool wholename;
};

static bool eval_name(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  struct name_data *name = data;
  w_string_t *str;

  if (name->wholename) {
    str = w_query_ctx_get_wholename(ctx);
  } else {
    str = file->name;
  }

  if (name->map) {
    bool matched;
    w_ht_val_t val;

    if (name->caseless) {
      str = w_string_dup_lower(str);
      if (!str) {
        return false;
      }
    }

    matched = w_ht_lookup(name->map, (w_ht_val_t)str, &val, false);

    if (name->caseless) {
      w_string_delref(str);
    }

    return matched;
  }

  if (name->caseless) {
    return w_string_equal_caseless(str, name->name);
  }
  return w_string_equal(str, name->name);
}

static void dispose_name(void *data)
{
  struct name_data *name = data;

  w_string_delref(name->name);
  free(name);
}

static w_query_expr *name_parser(w_query *query,
    json_t *term, bool caseless)
{
  const char *pattern, *scope = "basename";
  const char *which = caseless ? "iname" : "name";
  struct name_data *data;
  json_t *name;
  w_ht_t *map = NULL;

  if (!json_is_array(term)) {
    asprintf(&query->errmsg, "Expected array for '%s' term",
        which);
    return NULL;
  }

  if (json_array_size(term) > 3) {
    asprintf(&query->errmsg, "Invalid number of arguments for '%s' term",
        which);
    return NULL;
  }

  if (json_array_size(term) == 3) {
    json_t *jscope;

    jscope = json_array_get(term, 2);
    if (!json_is_string(jscope)) {
      asprintf(&query->errmsg,
          "Argument 3 to '%s' must be a string",
          which);
      return NULL;
    }

    scope = json_string_value(jscope);

    if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
      asprintf(&query->errmsg,
          "Invalid scope '%s' for %s expression",
          scope, which);
      return NULL;
    }
  }

  name = json_array_get(term, 1);

  if (json_is_array(name)) {
    uint32_t i;

    for (i = 0; i < json_array_size(name); i++) {
      if (!json_is_string(json_array_get(name, i))) {
        asprintf(&query->errmsg,
          "Argument 2 to '%s' must be either a string or an array of string",
          which);
        return NULL;
      }
    }

    map = w_ht_new(json_array_size(name), &w_ht_string_funcs);
    for (i = 0; i < json_array_size(name); i++) {
      w_string_t *element;
      const char *ele;

      ele = json_string_value(json_array_get(name, i));
      if (caseless) {
        element = w_string_new_lower(ele);
      } else {
        element = w_string_new(ele);
      }

      w_ht_set(map, (w_ht_val_t)element, 1);
      w_string_delref(element);
    }

  } else if (json_is_string(name)) {
    pattern = json_string_value(name);
  } else {
    asprintf(&query->errmsg,
        "Argument 2 to '%s' must be either a string or an array of string",
        which);
    return NULL;
  }


  data = calloc(1, sizeof(*data));
  if (pattern) {
    data->name = w_string_new(pattern);
  }
  data->map = map;
  data->caseless = caseless;
  data->wholename = !strcmp(scope, "wholename");

  return w_query_expr_new(eval_name, dispose_name, data);
}

w_query_expr *w_expr_name_parser(w_query *query, json_t *term)
{
  return name_parser(query, term, false);
}

w_query_expr *w_expr_iname_parser(w_query *query, json_t *term)
{
  return name_parser(query, term, true);
}

/* vim:ts=2:sw=2:et:
 */

