/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman_query.h"

#include <memory>

class ExistsExpr : public QueryExpr {
 public:
  EvaluateResult evaluate(QueryContext*, FileResult* file) override {
    return file->exists();
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref&) {
    return std::make_unique<ExistsExpr>();
  }
};
W_TERM_PARSER("exists", ExistsExpr::parse)

class EmptyExpr : public QueryExpr {
 public:
  EvaluateResult evaluate(QueryContext*, FileResult* file) override {
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
