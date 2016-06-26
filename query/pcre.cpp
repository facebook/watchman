/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_PCRE_H
struct match_pcre {
  pcre *re;
  pcre_extra *extra;
  bool wholename;
};

static bool eval_pcre(struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data)
{
  auto match = (match_pcre*)data;
  w_string_t *str;
  int rc;

  if (match->wholename) {
    str = w_query_ctx_get_wholename(ctx);
  } else {
    str = file->name;
  }

  rc = pcre_exec(match->re, match->extra,
        str->buf, str->len, 0, 0, NULL, 0);

  if (rc == PCRE_ERROR_NOMATCH) {
    return false;
  }
  if (rc >= 0) {
    return true;
  }
  // An error.  It's not actionable here
  return false;
}

static void dispose_pcre(void *data)
{
  auto match = (match_pcre*)data;

  if (match->re) {
    pcre_free(match->re);
  }
  if (match->extra) {
    pcre_free(match->extra);
  }
  free(match);
}

static w_query_expr *pcre_parser_inner(w_query *query,
    json_t *term, bool caseless)
{
  const char *ignore, *pattern, *scope = "basename";
  const char *which = caseless ? "ipcre" : "pcre";
  struct match_pcre *data;
  pcre *re;
  const char *errptr = NULL;
  int erroff = 0;
  int errcode = 0;

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

  re = pcre_compile2(pattern, caseless ? PCRE_CASELESS : 0,
        &errcode, &errptr, &erroff, NULL);
  if (!re) {
    ignore_result(asprintf(&query->errmsg,
      "invalid %s: code %d %s at offset %d in %s",
      which, errcode, errptr, erroff, pattern));
    return NULL;
  }

  data = (match_pcre *)malloc(sizeof(*data));
  data->re = re;
  data->extra = pcre_study(re, 0, &errptr);
  data->wholename = !strcmp(scope, "wholename");

  return w_query_expr_new(eval_pcre, dispose_pcre, data);
}

static w_query_expr *pcre_parser(w_query *query, json_t *term)
{
  return pcre_parser_inner(query, term, !query->case_sensitive);
}
W_TERM_PARSER("pcre", pcre_parser)

static w_query_expr *ipcre_parser(w_query *query, json_t *term)
{
  return pcre_parser_inner(query, term, true);
}
W_TERM_PARSER("ipcre", ipcre_parser)

#endif

/* vim:ts=2:sw=2:et:
 */
