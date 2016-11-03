/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

class NameExpr : public QueryExpr {
  w_string name;
  std::unordered_set<w_string> set;
  bool caseless;
  bool wholename;
  explicit NameExpr(
      std::unordered_set<w_string>&& set,
      bool caseless,
      bool wholename)
      : set(std::move(set)), caseless(caseless), wholename(wholename) {}

 public:
  bool evaluate(struct w_query_ctx* ctx, const watchman_file* file) override {
    w_string_t* str;

    if (wholename) {
      str = w_query_ctx_get_wholename(ctx);
    } else {
      str = w_file_get_name(file);
    }

    if (!set.empty()) {
      bool matched;

      if (caseless) {
        str = w_string_dup_lower(str);
        if (!str) {
          return false;
        }
      }

      matched = set.find(str) != set.end();

      if (caseless) {
        w_string_delref(str);
      }

      return matched;
    }

    if (caseless) {
      return w_string_equal_caseless(str, name);
    }
    return w_string_equal(str, name);
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query* query, const json_ref& term, bool caseless) {
    const char *pattern = nullptr, *scope = "basename";
    const char* which = caseless ? "iname" : "name";
    std::unordered_set<w_string> set;

    if (!json_is_array(term)) {
      ignore_result(
          asprintf(&query->errmsg, "Expected array for '%s' term", which));
      return nullptr;
    }

    if (json_array_size(term) > 3) {
      ignore_result(asprintf(
          &query->errmsg, "Invalid number of arguments for '%s' term", which));
      return nullptr;
    }

    if (json_array_size(term) == 3) {
      const auto& jscope = term.at(2);
      if (!json_is_string(jscope)) {
        ignore_result(asprintf(
            &query->errmsg, "Argument 3 to '%s' must be a string", which));
        return nullptr;
      }

      scope = json_string_value(jscope);

      if (strcmp(scope, "basename") && strcmp(scope, "wholename")) {
        ignore_result(asprintf(
            &query->errmsg,
            "Invalid scope '%s' for %s expression",
            scope,
            which));
        return nullptr;
      }
    }

    const auto& name = term.at(1);

    if (json_is_array(name)) {
      uint32_t i;

      for (i = 0; i < json_array_size(name); i++) {
        if (!json_is_string(json_array_get(name, i))) {
          ignore_result(asprintf(
              &query->errmsg,
              "Argument 2 to '%s' must be "
              "either a string or an "
              "array of string",
              which));
          return nullptr;
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
        if (caseless) {
          element =
              w_string_new_lower_typed(ele, json_to_w_string(jele).type());
        } else {
          element = w_string_new_typed(ele, json_to_w_string(jele).type());
        }

        w_string_in_place_normalize_separators(&element, WATCHMAN_DIR_SEP);

        set.insert(element);
        w_string_delref(element);
      }

    } else if (json_is_string(name)) {
      pattern = json_string_value(name);
    } else {
      ignore_result(asprintf(
          &query->errmsg,
          "Argument 2 to '%s' must be either a string or an array of string",
          which));
      return nullptr;
    }

    auto data =
        new NameExpr(std::move(set), caseless, !strcmp(scope, "wholename"));

    if (pattern) {
      // We need to make a copy of the string since we do in-place separator
      // normalization on the paths.
      w_string pat(pattern, json_to_w_string(name).type());
      data->name = w_string_normalize_separators(pat, WATCHMAN_DIR_SEP);
    }

    return std::unique_ptr<QueryExpr>(data);
  }

  static std::unique_ptr<QueryExpr> parseName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, !query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, true);
  }
};

W_TERM_PARSER("name", NameExpr::parseName)
W_TERM_PARSER("iname", NameExpr::parseIName)

/* vim:ts=2:sw=2:et:
 */
