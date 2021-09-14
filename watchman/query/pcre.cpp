/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include "watchman/Errors.h"
#include "watchman/query/FileResult.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/query/QueryExpr.h"

#include "watchman/watchman_query.h"

#ifdef HAVE_PCRE_H

using namespace watchman;

class PcreExpr : public QueryExpr {
  pcre* re;
  pcre_extra* extra;
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

  EvaluateResult evaluate(QueryContextBase* ctx, FileResult* file) override {
    w_string_piece str;
    int rc;

    if (wholename) {
      str = ctx->getWholeName();
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
  parse(Query*, const json_ref& term, CaseSensitivity caseSensitive) {
    const char *pattern, *scope = "basename";
    const char* which =
        caseSensitive == CaseSensitivity::CaseInSensitive ? "ipcre" : "pcre";
    pcre* re;
    const char* errptr = nullptr;
    int erroff = 0;
    int errcode = 0;

    if (term.array().size() > 1 && term.at(1).isString()) {
      pattern = json_string_value(term.at(1));
    } else {
      throw QueryParseError(folly::to<std::string>(
          "First parameter to \"", which, "\" term must be a pattern string"));
    }

    if (term.array().size() > 2) {
      if (term.at(2).isString()) {
        scope = json_string_value(term.at(2));
      } else {
        throw QueryParseError(folly::to<std::string>(
            "Second parameter to \"",
            which,
            "\" term must be an optional scope string"));
      }
    }

    if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
      throw QueryParseError(folly::to<std::string>(
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
      throw QueryParseError(folly::to<std::string>(
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

    return std::make_unique<PcreExpr>(
        re, pcre_study(re, 0, &errptr), !strcmp(scope, "wholename"));
  }
  static std::unique_ptr<QueryExpr> parsePcre(
      Query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIPcre(
      Query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};
W_TERM_PARSER("pcre", PcreExpr::parsePcre)
W_TERM_PARSER("ipcre", PcreExpr::parseIPcre)

#endif

/* vim:ts=2:sw=2:et:
 */
