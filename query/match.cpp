/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include <string>
using watchman::CaseSensitivity;

class WildMatchExpr : public QueryExpr {
  std::string pattern;
  CaseSensitivity caseSensitive;
  bool wholename;
  bool noescape;
  bool includedotfiles;

 public:
  WildMatchExpr(
      const char* pat,
      CaseSensitivity caseSensitive,
      bool wholename,
      bool noescape,
      bool includedotfiles)
      : pattern(pat),
        caseSensitive(caseSensitive),
        wholename(wholename),
        noescape(noescape),
        includedotfiles(includedotfiles) {}

  bool evaluate(struct w_query_ctx* ctx, const FileResult* file) override {
    w_string_piece str;
    bool res;

    if (wholename) {
      str = w_query_ctx_get_wholename(ctx);
    } else {
      str = file->baseName();
    }

#ifdef _WIN32
    // Translate to unix style slashes for wildmatch
    w_string normBuf = str.asWString().normalizeSeparators();
    str = normBuf;
#endif

    res = wildmatch(pattern.c_str(), str.data(),
                    (includedotfiles ? 0 : WM_PERIOD) |
                        (noescape ? WM_NOESCAPE : 0) |
                        (wholename ? WM_PATHNAME : 0) |
                        (caseSensitive == CaseSensitivity::CaseInSensitive
                             ? WM_CASEFOLD
                             : 0),
                    0) == WM_MATCH;

    return res;
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query*, const json_ref& term, CaseSensitivity case_sensitive) {
    const char *ignore, *pattern, *scope = "basename";
    const char *which =
        case_sensitive == CaseSensitivity::CaseInSensitive ? "imatch" : "match";
    int noescape = 0;
    int includedotfiles = 0;

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
      throw QueryParseError(
          "Expected [\"", which, "\", \"pattern\", \"scope\"?]");
    }

    if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
      throw QueryParseError(
          "Invalid scope '", scope, "' for ", which, " expression");
    }

    return watchman::make_unique<WildMatchExpr>(
        pattern,
        case_sensitive,
        !strcmp(scope, "wholename"),
        noescape,
        includedotfiles);
  }
  static std::unique_ptr<QueryExpr> parseMatch(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIMatch(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};
W_TERM_PARSER("match", WildMatchExpr::parseMatch)
W_TERM_PARSER("imatch", WildMatchExpr::parseIMatch)
W_CAP_REG("wildmatch")
W_CAP_REG("wildmatch-multislash")

/* vim:ts=2:sw=2:et:
 */
