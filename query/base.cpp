/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include <vector>
#include "make_unique.h"

/* Basic boolean and compound expressions */

class NotExpr : public QueryExpr {
  std::unique_ptr<QueryExpr> expr;

 public:
  explicit NotExpr(std::unique_ptr<QueryExpr> other_expr)
      : expr(std::move(other_expr)) {}

  bool evaluate(w_query_ctx* ctx, const FileResult* file) override {
    return !expr->evaluate(ctx, file);
  }
  static std::unique_ptr<QueryExpr> parse(
      w_query* query,
      const json_ref& term) {
    /* rigidly require ["not", expr] */
    if (!json_is_array(term) || json_array_size(term) != 2) {
      throw QueryParseError("must use [\"not\", expr]");
    }

    const auto& other = term.at(1);
    auto other_expr = w_query_expr_parse(query, other);
    return watchman::make_unique<NotExpr>(std::move(other_expr));
  }
};

W_TERM_PARSER("not", NotExpr::parse)

class TrueExpr : public QueryExpr {
 public:
  bool evaluate(w_query_ctx*, const FileResult*) override {
    return true;
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return watchman::make_unique<TrueExpr>();
  }
};

W_TERM_PARSER("true", TrueExpr::parse)

class FalseExpr : public QueryExpr {
 public:
  bool evaluate(w_query_ctx*, const FileResult*) override {
    return false;
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return watchman::make_unique<FalseExpr>();
  }
};

W_TERM_PARSER("false", FalseExpr::parse)

class ListExpr : public QueryExpr {
  bool allof;
  std::vector<std::unique_ptr<QueryExpr>> exprs;

 public:
  ListExpr(bool isAll, std::vector<std::unique_ptr<QueryExpr>> exprs)
      : allof(isAll), exprs(std::move(exprs)) {}

  bool evaluate(w_query_ctx* ctx, const FileResult* file) override {
    for (auto& expr : exprs) {
      bool res = expr->evaluate(ctx, file);

      if (!res && allof) {
        return false;
      }

      if (res && !allof) {
        return true;
      }
    }
    return allof;
  }

  static std::unique_ptr<QueryExpr>
  parse(w_query* query, const json_ref& term, bool allof) {
    std::vector<std::unique_ptr<QueryExpr>> list;

    /* don't allow "allof" on its own */
    if (!json_is_array(term) || json_array_size(term) < 2) {
      if (allof) {
        throw QueryParseError("must use [\"allof\", expr...]");
      }
      throw QueryParseError("must use [\"anyof\", expr...]");
    }

    auto n = json_array_size(term) - 1;
    list.reserve(n);

    for (size_t i = 0; i < n; i++) {
      const auto& exp = term.at(i + 1);

      auto parsed = w_query_expr_parse(query, exp);
      list.emplace_back(std::move(parsed));
    }

    return watchman::make_unique<ListExpr>(allof, std::move(list));
  }

  static std::unique_ptr<QueryExpr> parseAllOf(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, true);
  }
  static std::unique_ptr<QueryExpr> parseAnyOf(
      w_query* query,
      const json_ref& term) {
    return parse(query, term, false);
  }
};

W_TERM_PARSER("anyof", ListExpr::parseAnyOf)
W_TERM_PARSER("allof", ListExpr::parseAllOf)

/* vim:ts=2:sw=2:et:
 */
