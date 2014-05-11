/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifndef FNM_CASEFOLD
# define FNM_CASEFOLD 0
# define NO_CASELESS_FNMATCH 1
#endif

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

static w_query_expr *match_parser_inner(w_query *query,
    json_t *term, bool caseless)
{
  const char *ignore, *pattern, *scope = "basename";
  const char *which = caseless ? "imatch" : "match";
  struct match_data *data;

  if (json_unpack(term, "[s,s,s]", &ignore, &pattern, &scope) != 0 &&
      json_unpack(term, "[s,s]", &ignore, &pattern) != 0) {
    ignore_result(asprintf(&query->errmsg,
        "Expected [\"%s\", \"pattern\", \"scope\"?]",
        which));
    return NULL;
  }

  if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
    ignore_result(asprintf(&query->errmsg,
        "Invalid scope '%s' for %s expression",
        scope, which));
    return NULL;
  }

  data = malloc(sizeof(*data));
  data->pattern = strdup(pattern);
  data->caseless = caseless;
  data->wholename = !strcmp(scope, "wholename");

  return w_query_expr_new(eval_match, dispose_match, data);
}

static w_query_expr *match_parser(w_query *query, json_t *term)
{
  return match_parser_inner(query, term, false);
}
W_TERM_PARSER("match", match_parser)

static w_query_expr *imatch_parser(w_query *query, json_t *term)
{
#ifdef NO_CASELESS_FNMATCH
  unused_parameter(term);
  asprintf(&query->errmsg,
      "imatch: Your system doesn't support FNM_CASEFOLD");
  return NULL;
#else
  return match_parser_inner(query, term, true);
#endif
}
W_TERM_PARSER("imatch", imatch_parser)

/* vim:ts=2:sw=2:et:
 */
