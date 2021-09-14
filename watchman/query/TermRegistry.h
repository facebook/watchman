/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_preprocessor.h"

namespace watchman {

struct Query;
class QueryExpr;

typedef std::unique_ptr<QueryExpr> (
    *QueryExprParser)(Query* query, const json_ref& term);

bool registerExpressionParser(const char* term, QueryExprParser parser);
QueryExprParser getQueryExprParser(const w_string& name);

#define W_TERM_PARSER1(symbol, name, func) \
  static w_ctor_fn_type(symbol) {          \
    registerExpressionParser(name, func);  \
  }                                        \
  w_ctor_fn_reg(symbol)

#define W_TERM_PARSER(name, func) \
  W_TERM_PARSER1(w_gen_symbol(w_term_register_), name, func)

} // namespace watchman
