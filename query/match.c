/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct match_data {
  char *pattern;
  bool caseless;
  bool wholename;
};

static bool eval_match(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  struct match_data *match = data;
  w_string_t *str;

  if (match->wholename) {
    str = w_query_ctx_get_wholename(ctx);
  } else {
    str = file->name;
  }

  return fnmatch(match->pattern,
      str->buf,
      FNM_PERIOD |
      (match->caseless ? FNM_CASEFOLD : 0)) == 0;
}

static void dispose_match(void *data)
{
  struct match_data *match = data;

  free(match->pattern);
  free(match);
}

static w_query_expr *match_parser(w_query *query,
    json_t *term, bool caseless)
{
  const char *ignore, *pattern, *scope = "basename";
  const char *which = caseless ? "imatch" : "match";
  struct match_data *data;

  if (json_unpack(term, "[s,s,s]", &ignore, &pattern, &scope) != 0 &&
      json_unpack(term, "[s,s]", &ignore, &pattern) != 0) {
    asprintf(&query->errmsg,
        "Expected [\"%s\", \"pattern\", \"scope\"?]",
        which);
    return NULL;
  }

  if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
    asprintf(&query->errmsg,
        "Invalid scope '%s' for %s expression",
        scope, which);
    return NULL;
  }

  data = malloc(sizeof(*data));
  data->pattern = strdup(pattern);
  data->caseless = caseless;
  data->wholename = !strcmp(scope, "wholename");

  return w_query_expr_new(eval_match, dispose_match, data);
}

w_query_expr *w_expr_match_parser(w_query *query, json_t *term)
{
  return match_parser(query, term, false);
}

w_query_expr *w_expr_imatch_parser(w_query *query, json_t *term)
{
  return match_parser(query, term, true);
}

/* vim:ts=2:sw=2:et:
 */

