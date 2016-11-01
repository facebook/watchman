/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

class ExistsExpr : public QueryExpr {
 public:
  bool evaluate(struct w_query_ctx*, const watchman_file* file) override {
    return file->exists;
  }
  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return watchman::make_unique<ExistsExpr>();
  }
};
W_TERM_PARSER("exists", ExistsExpr::parse)

class EmptyExpr : public QueryExpr {
 public:
  bool evaluate(struct w_query_ctx*, const watchman_file* file) override {
    if (!file->exists) {
      return false;
    }

    if (S_ISDIR(file->stat.mode) || S_ISREG(file->stat.mode)) {
      return file->stat.size == 0;
    }

    return false;
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return watchman::make_unique<EmptyExpr>();
  }
};
W_TERM_PARSER("empty", EmptyExpr::parse)

/* vim:ts=2:sw=2:et:
 */
