/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/watchman.h"

using namespace watchman;

class NameExpr : public QueryExpr {
  w_string name;
  std::unordered_set<w_string> set;
  CaseSensitivity caseSensitive;
  bool wholename;
  explicit NameExpr(
      std::unordered_set<w_string>&& set,
      CaseSensitivity caseSensitive,
      bool wholename)
      : set(std::move(set)),
        caseSensitive(caseSensitive),
        wholename(wholename) {}

 public:
  EvaluateResult evaluate(struct w_query_ctx* ctx, FileResult* file) override {
    if (!set.empty()) {
      bool matched;
      w_string str;

      if (wholename) {
        str = w_query_ctx_get_wholename(ctx);
        if (caseSensitive == CaseSensitivity::CaseInSensitive) {
          str = str.piece().asLowerCase();
        }
      } else {
        str = caseSensitive == CaseSensitivity::CaseInSensitive
            ? file->baseName().asLowerCase()
            : file->baseName().asWString();
      }

      matched = set.find(str) != set.end();

      return matched;
    }

    w_string_piece str;

    if (wholename) {
      str = w_query_ctx_get_wholename(ctx);
    } else {
      str = file->baseName();
    }

    if (caseSensitive == CaseSensitivity::CaseInSensitive) {
      return w_string_equal_caseless(str, name);
    }
    return str == name;
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query*, const json_ref& term, CaseSensitivity caseSensitive) {
    const char *pattern = nullptr, *scope = "basename";
    const char* which =
        caseSensitive == CaseSensitivity::CaseInSensitive ? "iname" : "name";
    std::unordered_set<w_string> set;

    if (!term.isArray()) {
      throw QueryParseError("Expected array for '", which, "' term");
    }

    if (json_array_size(term) > 3) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    if (json_array_size(term) == 3) {
      const auto& jscope = term.at(2);
      if (!jscope.isString()) {
        throw QueryParseError("Argument 3 to '", which, "' must be a string");
      }

      scope = json_string_value(jscope);

      if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
        throw QueryParseError(
            "Invalid scope '", scope, "' for ", which, " expression");
      }
    }

    const auto& name = term.at(1);

    if (name.isArray()) {
      uint32_t i;

      for (i = 0; i < json_array_size(name); i++) {
        if (!json_array_get(name, i).isString()) {
          throw QueryParseError(
              "Argument 2 to '",
              which,
              "' must be either a string or an array of string");
        }
      }

      set.reserve(json_array_size(name));
      for (i = 0; i < json_array_size(name); i++) {
        w_string element;
        const auto& jele = name.at(i);
        auto ele = json_to_w_string(jele);

        if (caseSensitive == CaseSensitivity::CaseInSensitive) {
          element = ele.piece().asLowerCase(ele.type()).normalizeSeparators();
        } else {
          element = ele.normalizeSeparators();
        }

        set.insert(element);
      }

    } else if (name.isString()) {
      pattern = json_string_value(name);
    } else {
      throw QueryParseError(
          "Argument 2 to '",
          which,
          "' must be either a string or an array of string");
    }

    auto data = new NameExpr(
        std::move(set), caseSensitive, !strcmp(scope, "wholename"));

    if (pattern) {
      data->name = json_to_w_string(name).normalizeSeparators();
    }

    return std::unique_ptr<QueryExpr>(data);
  }

  static std::unique_ptr<QueryExpr> parseName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};

W_TERM_PARSER("name", NameExpr::parseName)
W_TERM_PARSER("iname", NameExpr::parseIName)

/* vim:ts=2:sw=2:et:
 */
