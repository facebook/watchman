/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/TermRegistry.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"

namespace watchman {

// This can't be a simple global because other compilation units
// may try to mutate it before this compilation unit has had its
// constructors run, leading to SIOF.
static std::unordered_map<w_string, QueryExprParser>& term_hash() {
  static std::unordered_map<w_string, QueryExprParser> hash;
  return hash;
}

bool registerExpressionParser(const char* term, QueryExprParser parser) {
  char capname[128];
  w_string name(term, W_STRING_UNICODE);

  snprintf(capname, sizeof(capname), "term-%s", term);
  capability_register(capname);

  term_hash()[name] = parser;
  return true;
}

QueryExprParser getQueryExprParser(const w_string& name) {
  auto it = term_hash().find(name);
  if (it == term_hash().end()) {
    throw QueryParseError(
        folly::to<std::string>("unknown expression term '", name.view(), "'"));
  }
  return it->second;
}

} // namespace watchman
