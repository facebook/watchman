/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/watchman.h"

#include <memory>

class ExistsExpr : public QueryExpr {
 public:
  EvaluateResult evaluate(struct w_query_ctx*, FileResult* file) override {
    return file->exists();
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return std::make_unique<ExistsExpr>();
  }
};
W_TERM_PARSER("exists", ExistsExpr::parse)

class EmptyExpr : public QueryExpr {
 public:
  EvaluateResult evaluate(struct w_query_ctx*, FileResult* file) override {
    auto exists = file->exists();
    auto stat = file->stat();
    auto size = file->size();

    if (!exists.has_value()) {
      return folly::none;
    }
    if (!exists.value()) {
      return false;
    }

    if (!stat.has_value()) {
      return folly::none;
    }

    if (!size.has_value()) {
      return folly::none;
    }

    if (stat->isDir() || stat->isFile()) {
      return size.value() == 0;
    }

    return false;
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return std::make_unique<EmptyExpr>();
  }
};
W_TERM_PARSER("empty", EmptyExpr::parse)

/* vim:ts=2:sw=2:et:
 */
