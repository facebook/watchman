/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

using watchman::CaseSensitivity;

static inline bool is_dir_sep(int c) {
  return c == '/' || c == '\\';
}

class DirNameExpr : public QueryExpr {
  w_string dirname;
  struct w_query_int_compare depth;
  using StartsWith = bool (*)(w_string_t* str, w_string_t* prefix);
  StartsWith startswith;

 public:
  explicit DirNameExpr(
      w_string dirname,
      struct w_query_int_compare depth,
      StartsWith startswith)
      : dirname(dirname), depth(depth), startswith(startswith) {}

  bool evaluate(w_query_ctx* ctx, const FileResult*) override {
    auto& str = w_query_ctx_get_wholename(ctx);
    size_t i;

    if (str.size() <= dirname.size()) {
      // Either it doesn't prefix match, or file name is == dirname.
      // That means that the best case is that the wholename matches.
      // we only want to match if dirname(wholename) matches, so it
      // is not possible for us to match unless the length of wholename
      // is greater than the dirname operand
      return false;
    }

    // Want to make sure that wholename is a child of dirname, so
    // check for a dir separator.  Special case for dirname == '' (the root),
    // which won't have a slash in position 0.
    if (dirname.size() > 0 && !is_dir_sep(str.data()[dirname.size()])) {
      // may have a common prefix with, but is not a child of dirname
      return false;
    }

    if (!startswith(str, dirname)) {
      return false;
    }

    // Now compute the depth of file from dirname.  We do this by
    // counting dir separators, not including the one we saw above.
    json_int_t actual_depth = 0;
    for (i = dirname.size() + 1; i < str.size(); i++) {
      if (is_dir_sep(str.data()[i])) {
        actual_depth++;
      }
    }

    return eval_int_compare(actual_depth, &depth);
  }

  // ["dirname", "foo"] -> ["dirname", "foo", ["depth", "ge", 0]]
  static std::unique_ptr<QueryExpr>
  parse(w_query*, const json_ref& term, CaseSensitivity case_sensitive) {
    const char *which = case_sensitive == CaseSensitivity::CaseInSensitive
                            ? "idirname"
                            : "dirname";
    struct w_query_int_compare depth_comp;

    if (!json_is_array(term)) {
      throw QueryParseError("Expected array for '", which, "' term");
    }

    if (json_array_size(term) < 2) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    if (json_array_size(term) > 3) {
      throw QueryParseError(
          "Invalid number of arguments for '", which, "' term");
    }

    const auto& name = term.at(1);
    if (!json_is_string(name)) {
      throw QueryParseError("Argument 2 to '", which, "' must be a string");
    }

    if (json_array_size(term) == 3) {
      const auto& depth = term.at(2);
      if (!json_is_array(depth)) {
        throw QueryParseError(
            "Invalid number of arguments for '", which, "' term");
      }

      parse_int_compare(depth, &depth_comp);

      if (strcmp("depth", json_string_value(json_array_get(depth, 0)))) {
        throw QueryParseError(
            "Third parameter to '",
            which,
            "' should be a relational depth term");
      }
    } else {
      depth_comp.operand = 0;
      depth_comp.op = W_QUERY_ICMP_GE;
    }

    return watchman::make_unique<DirNameExpr>(
        json_to_w_string(name), depth_comp,
        case_sensitive == CaseSensitivity::CaseInSensitive
            ? w_string_startswith_caseless
            : w_string_startswith);
  }
  static std::unique_ptr<QueryExpr> parseDirName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, query->case_sensitive);
  }
  static std::unique_ptr<QueryExpr> parseIDirName(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, CaseSensitivity::CaseInSensitive);
  }
};

W_TERM_PARSER("dirname", DirNameExpr::parseDirName)
W_TERM_PARSER("idirname", DirNameExpr::parseIDirName)

/* vim:ts=2:sw=2:et:
 */
