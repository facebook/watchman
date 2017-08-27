/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

class SuffixExpr : public QueryExpr {
  std::unordered_set<w_string> suffixSet_;

 public:
  explicit SuffixExpr(std::unordered_set<w_string>&& suffixSet)
      : suffixSet_(std::move(suffixSet)) {}

  bool evaluate(struct w_query_ctx*, const FileResult* file) override {
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

    if (!json_is_array(term)) {
      throw QueryParseError("Expected array for 'suffix' term");
    }

    if (json_array_size(term) > 2) {
      throw QueryParseError("Invalid number of arguments for 'suffix' term");
    }

    const auto& suffix = term.at(1);

    // Suffix match supports array or single suffix string
    if (json_is_array(suffix)) {
      suffixSet.reserve(json_array_size(suffix));
      for (const auto& ele : suffix.array()) {
        if (!json_is_string(ele)) {
          throw QueryParseError(
              "Argument 2 to 'suffix' must be either a string or an array of string");
        }
        suffixSet.insert(json_to_w_string(ele).piece().asLowerCase());
      }
    } else if (json_is_string(suffix)) {
      suffixSet.insert(json_to_w_string(suffix).piece().asLowerCase());
    } else {
      throw QueryParseError(
          "Argument 2 to 'suffix' must be either a string or an array of string");
    }
    return watchman::make_unique<SuffixExpr>(std::move(suffixSet));
  }
};
W_TERM_PARSER("suffix", SuffixExpr::parse)
W_CAP_REG("suffix-set")

/* vim:ts=2:sw=2:et:
 */
