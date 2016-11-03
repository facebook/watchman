/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

class SuffixExpr : public QueryExpr {
  w_string suffix;

 public:
  explicit SuffixExpr(w_string suffix) : suffix(suffix) {}

  bool evaluate(struct w_query_ctx*, const watchman_file* file) {
    return w_string_suffix_match(w_file_get_name(file), suffix);
  }

  static std::unique_ptr<QueryExpr> parse(
      w_query* query,
      const json_ref& term) {
    const char *ignore, *suffix;

    if (json_unpack(term, "[s,s]", &ignore, &suffix) != 0) {
      query->errmsg = strdup("must use [\"suffix\", \"suffixstring\"]");
      return nullptr;
    }

    w_string str(w_string_new_lower_typed(suffix, W_STRING_BYTE), false);
    if (!str) {
      query->errmsg = strdup("out of memory");
      return nullptr;
    }

    return watchman::make_unique<SuffixExpr>(str);
  }
};
W_TERM_PARSER("suffix", SuffixExpr::parse)

/* vim:ts=2:sw=2:et:
 */
