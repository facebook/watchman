/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/wildmatch/wildmatch.h"

struct wildmatch_data {
  char *pattern;
  bool caseless;
  bool wholename;
  bool noescape;
  bool includedotfiles;
};

static bool eval_wildmatch(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  struct wildmatch_data *match = data;
  w_string_t *str;

  if (match->wholename) {
    str = w_query_ctx_get_wholename(ctx);
  } else {
    str = file->name;
  }

  return wildmatch(
    match->pattern,
    str->buf,
    (match->includedotfiles ? 0 : WM_PERIOD) |
    (match->noescape ? WM_NOESCAPE : 0) |
    (match->wholename ? WM_PATHNAME : 0) |
    (match->caseless ? WM_CASEFOLD : 0),
    0) == WM_MATCH;
}

static void dispose_match(void *data)
{
  struct wildmatch_data *match = data;

  free(match->pattern);
  free(match);
}

static w_query_expr *wildmatch_parser_inner(w_query *query,
    json_t *term, bool caseless)
{
  const char *ignore, *pattern, *scope = "basename";
  const char *which = caseless ? "imatch" : "match";
  int noescape = 0;
  int includedotfiles = 0;
  struct wildmatch_data *data;

  if (json_unpack(
        term,
        "[s,s,s,{s?b,s?b}]",
        &ignore,
        &pattern,
        &scope,
        "noescape",
        &noescape,
        "includedotfiles",
        &includedotfiles) != 0 &&
      json_unpack(term, "[s,s,s]", &ignore, &pattern, &scope) != 0 &&
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
  data->noescape = noescape;
  data->includedotfiles = includedotfiles;

  return w_query_expr_new(eval_wildmatch, dispose_match, data);
}

static w_query_expr *wildmatch_parser(w_query *query, json_t *term)
{
  return wildmatch_parser_inner(query, term, !query->case_sensitive);
}
W_TERM_PARSER("match", wildmatch_parser)

static w_query_expr *iwildmatch_parser(w_query *query, json_t *term)
{
  return wildmatch_parser_inner(query, term, true);
}
W_TERM_PARSER("imatch", iwildmatch_parser)

/* vim:ts=2:sw=2:et:
 */
