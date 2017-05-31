/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
using watchman::CaseSensitivity;

class NameExpr : public QueryExpr {
  w_string name;
  std::unordered_set<w_string> set;
  CaseSensitivity caseSensitive;
  bool wholename;
  explicit NameExpr(
      std::unordered_set<w_string>&& set,
      CaseSensitivity caseSensitive,
      bool wholename)
      : set(std::move(set)), caseSensitive(caseSensitive), wholename(wholename) {}

 public:
  bool evaluate(struct w_query_ctx* ctx, const FileResult* file) override {
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
    const char *which =
        caseSensitive == CaseSensitivity::CaseInSensitive ? "iname" : "name";
    std::unordered_set<w_string> set;

    if (!json_is_array(term)) {
      throw QueryParseError("Expected array for '", which, "' term");
    }

    if (json_array_size(term) > 3) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    if (json_array_size(term) == 3) {
      const auto& jscope = term.at(2);
      if (!json_is_string(jscope)) {
        throw QueryParseError("Argument 3 to '", which, "' must be a string");
      }

      scope = json_string_value(jscope);

      if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
        throw QueryParseError(
            "Invalid scope '", scope, "' for ", which, " expression");
      }
    }

    const auto& name = term.at(1);

    if (json_is_array(name)) {
      uint32_t i;

      for (i = 0; i < json_array_size(name); i++) {
        if (!json_is_string(json_array_get(name, i))) {
          throw QueryParseError(
              "Argument 2 to '",
              which,
              "' must be either a string or an array of string");
        }
      }

      set.reserve(json_array_size(name));
      for (i = 0; i < json_array_size(name); i++) {
        w_string_t* element;
        const char* ele;
        const auto& jele = name.at(i);
        ele = json_string_value(jele);
        // We need to make a copy of the string since we do in-place separator
        // normalization on the paths.
        if (caseSensitive == CaseSensitivity::CaseInSensitive) {
          element =
              w_string_new_lower_typed(ele, json_to_w_string(jele).type());
        } else {
          element = w_string_new_typed(ele, json_to_w_string(jele).type());
        }

        w_string_in_place_normalize_separators(&element);

        set.insert(element);
        w_string_delref(element);
      }

    } else if (json_is_string(name)) {
      pattern = json_string_value(name);
    } else {
      throw QueryParseError(
          "Argument 2 to '",
          which,
          "' must be either a string or an array of string");
    }

    auto data = new NameExpr(std::move(set), caseSensitive,
                             !strcmp(scope, "wholename"));

    if (pattern) {
      // We need to make a copy of the string since we do in-place separator
      // normalization on the paths.
      w_string pat(pattern, json_to_w_string(name).type());
      data->name = w_string_normalize_separators(pat);
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
