/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>
#include "watchman/Clock.h"
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"

namespace watchman {

struct QueryDebugInfo {
  std::vector<w_string> cookieFileNames;

  json_ref render() const;
};

struct QueryResult {
  bool isFreshInstance;
  json_ref resultsArray;
  // Only populated if the query was set to dedup_results
  std::unordered_set<w_string> dedupedFileNames;
  ClockSpec clockAtStartOfQuery;
  uint32_t stateTransCountAtStartOfQuery;
  json_ref savedStateInfo;
  QueryDebugInfo debugInfo;
};

} // namespace watchman
