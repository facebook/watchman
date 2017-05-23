/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

class SuffixExpr : public QueryExpr {
  w_string suffix;

 public:
  explicit SuffixExpr(w_string suffix) : suffix(suffix) {}

  bool evaluate(struct w_query_ctx*, const FileResult* file) override {
    return file->baseName().hasSuffix(suffix);
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref& term) {
    const char *ignore, *suffix;

    if (json_unpack(term, "[s,s]", &ignore, &suffix) != 0) {
      throw QueryParseError("must use [\"suffix\", \"suffixstring\"]");
    }

    w_string str(w_string_new_lower_typed(suffix, W_STRING_BYTE), false);
    return watchman::make_unique<SuffixExpr>(str);
  }
};
W_TERM_PARSER("suffix", SuffixExpr::parse)

/* vim:ts=2:sw=2:et:
 */
