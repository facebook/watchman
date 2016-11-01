/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include <string>

class WildMatchExpr : public QueryExpr {
  std::string pattern;
  bool caseless;
  bool wholename;
  bool noescape;
  bool includedotfiles;

 public:
  WildMatchExpr(
      const char* pat,
      bool caseless,
      bool wholename,
      bool noescape,
      bool includedotfiles)
      : pattern(pat),
        caseless(caseless),
        wholename(wholename),
        noescape(noescape),
        includedotfiles(includedotfiles) {}

  bool evaluate(struct w_query_ctx* ctx, const watchman_file* file) override {
    w_string_t* str;
    bool res;

    if (wholename) {
      str = w_query_ctx_get_wholename(ctx);
    } else {
      str = w_file_get_name(file);
    }

#ifdef _WIN32
    // Translate to unix style slashes for wildmatch
    str = w_string_normalize_separators(str, '/');
#endif

    res = wildmatch(
              pattern.c_str(),
              str->buf,
              (includedotfiles ? 0 : WM_PERIOD) | (noescape ? WM_NOESCAPE : 0) |
                  (wholename ? WM_PATHNAME : 0) | (caseless ? WM_CASEFOLD : 0),
              0) == WM_MATCH;

#ifdef _WIN32
    w_string_delref(str);
#endif

    return res;
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query* query, const json_ref& term, bool caseless) {
    const char *ignore, *pattern, *scope = "basename";
    const char* which = caseless ? "imatch" : "match";
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
      ignore_result(asprintf(
          &query->errmsg, "Expected [\"%s\", \"pattern\", \"scope\"?]", which));
      return nullptr;
    }

    if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
      ignore_result(asprintf(
          &query->errmsg,
          "Invalid scope '%s' for %s expression",
          scope,
          which));
      return nullptr;
    }

    return watchman::make_unique<WildMatchExpr>(
        pattern,
        caseless,
        !strcmp(scope, "wholename"),
        noescape,
        includedotfiles);
  }
  static std::unique_ptr<QueryExpr> parseMatch(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, !query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIMatch(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, true);
  }
};
W_TERM_PARSER("match", WildMatchExpr::parseMatch)
W_TERM_PARSER("imatch", WildMatchExpr::parseIMatch)
W_CAP_REG("wildmatch")
W_CAP_REG("wildmatch-multislash")

/* vim:ts=2:sw=2:et:
 */
