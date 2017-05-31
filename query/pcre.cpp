/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

#ifdef HAVE_PCRE_H

using watchman::CaseSensitivity;

class PcreExpr : public QueryExpr {
  pcre *re;
  pcre_extra *extra;
  bool wholename;

 public:
  explicit PcreExpr(pcre* re, pcre_extra* extra, bool wholename)
      : re(re), extra(extra), wholename(wholename) {}

  ~PcreExpr() override {
    if (re) {
      pcre_free(re);
    }
    if (extra) {
      pcre_free(extra);
    }
  }

  bool evaluate(struct w_query_ctx* ctx, const FileResult* file) override {
    w_string_piece str;
    int rc;

    if (wholename) {
      str = w_query_ctx_get_wholename(ctx);
    } else {
      str = file->baseName();
    }

    rc = pcre_exec(re, extra, str.data(), str.size(), 0, 0, nullptr, 0);

    if (rc == PCRE_ERROR_NOMATCH) {
      return false;
    }
    if (rc >= 0) {
      return true;
    }
    // An error.  It's not actionable here
    return false;
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query*, const json_ref& term, CaseSensitivity caseSensitive) {
    const char *ignore, *pattern, *scope = "basename";
    const char *which =
        caseSensitive == CaseSensitivity::CaseInSensitive ? "ipcre" : "pcre";
    pcre* re;
    const char* errptr = nullptr;
    int erroff = 0;
    int errcode = 0;

    if (json_unpack(term, "[s,s,s]", &ignore, &pattern, &scope) != 0 &&
        json_unpack(term, "[s,s]", &ignore, &pattern) != 0) {
      throw QueryParseError(watchman::to<std::string>(
          "Expected [\"", which, "\", \"pattern\", \"scope\"?]"));
    }

    if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
      throw QueryParseError(watchman::to<std::string>(
          "Invalid scope '", scope, "' for ", which, " expression"));
    }

    re = pcre_compile2(
        pattern,
        caseSensitive == CaseSensitivity::CaseInSensitive ? PCRE_CASELESS : 0,
        &errcode,
        &errptr,
        &erroff,
        nullptr);
    if (!re) {
      throw QueryParseError(watchman::to<std::string>(
          "invalid ",
          which,
          ": code ",
          errcode,
          " ",
          errptr,
          " at offset ",
          erroff,
          " in ",
          pattern));
    }

    return watchman::make_unique<PcreExpr>(
        re, pcre_study(re, 0, &errptr), !strcmp(scope, "wholename"));
  }
  static std::unique_ptr<QueryExpr> parsePcre(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIPcre(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};
W_TERM_PARSER("pcre", PcreExpr::parsePcre)
W_TERM_PARSER("ipcre", PcreExpr::parseIPcre)

#endif

/* vim:ts=2:sw=2:et:
 */
