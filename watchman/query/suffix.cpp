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

#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/watchman_query.h"

#include <memory>

using namespace watchman;

class SuffixExpr : public QueryExpr {
  std::unordered_set<w_string> suffixSet_;

 public:
  explicit SuffixExpr(std::unordered_set<w_string>&& suffixSet)
      : suffixSet_(std::move(suffixSet)) {}

  EvaluateResult evaluate(QueryContext*, FileResult* file) override {
    if (suffixSet_.size() < 3) {
      // For small suffix sets, benchmarks indicated that iteration provides
      // better performance since no suffix allocation is necessary.
      for (auto const& suffix : suffixSet_) {
        if (file->baseName().hasSuffix(suffix)) {
          return true;
        }
      }
      return false;
    }
    auto suffix = file->baseName().asLowerCaseSuffix();
    return suffix && (suffixSet_.find(suffix) != suffixSet_.end());
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref& term) {
    std::unordered_set<w_string> suffixSet;

    if (!term.isArray()) {
      throw QueryParseError("Expected array for 'suffix' term");
    }

    if (json_array_size(term) > 2) {
      throw QueryParseError("Invalid number of arguments for 'suffix' term");
    }

    const auto& suffix = term.at(1);

    // Suffix match supports array or single suffix string
    if (suffix.isArray()) {
      suffixSet.reserve(json_array_size(suffix));
      for (const auto& ele : suffix.array()) {
        if (!ele.isString()) {
          throw QueryParseError(
              "Argument 2 to 'suffix' must be either a string or an array of string");
        }
        suffixSet.insert(json_to_w_string(ele).piece().asLowerCase());
      }
    } else if (suffix.isString()) {
      suffixSet.insert(json_to_w_string(suffix).piece().asLowerCase());
    } else {
      throw QueryParseError(
          "Argument 2 to 'suffix' must be either a string or an array of string");
    }
    return std::make_unique<SuffixExpr>(std::move(suffixSet));
  }

  std::unique_ptr<QueryExpr> aggregate(
      const QueryExpr* other,
      const AggregateOp op) const override {
    if (op != AggregateOp::AnyOf) {
      return nullptr;
    }
    const SuffixExpr* otherExpr = dynamic_cast<const SuffixExpr*>(other);
    if (otherExpr == nullptr) {
      return nullptr;
    }
    std::unordered_set<w_string> suffixSet;
    suffixSet.reserve(suffixSet_.size() + otherExpr->suffixSet_.size());
    suffixSet.insert(
        otherExpr->suffixSet_.begin(), otherExpr->suffixSet_.end());
    suffixSet.insert(suffixSet_.begin(), suffixSet_.end());
    return std::make_unique<SuffixExpr>(std::move(suffixSet));
  }
};
W_TERM_PARSER("suffix", SuffixExpr::parse)
W_CAP_REG("suffix-set")

/* vim:ts=2:sw=2:et:
 */
