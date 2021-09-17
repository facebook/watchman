/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include "watchman/Clock.h"
#include "watchman/FileSystem.h"
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"

namespace watchman {

class FileResult;
struct GlobTree;
struct QueryContext;
class QueryExpr;

struct QueryFieldRenderer {
  w_string name;
  std::optional<json_ref> (*make)(FileResult* file, const QueryContext* ctx);
};

using QueryFieldList = std::vector<QueryFieldRenderer*>;

struct QueryPath {
  w_string name;
  int depth;
};

struct Query {
  CaseSensitivity case_sensitive = CaseSensitivity::CaseInSensitive;
  bool fail_if_no_saved_state = false;
  bool empty_on_fresh_instance = false;
  bool omit_changed_files = false;
  bool dedup_results = false;
  uint32_t bench_iterations = 0;

  /* optional full path to relative root, without and with trailing slash */
  w_string relative_root;
  w_string relative_root_slash;

  std::optional<std::vector<QueryPath>> paths;

  std::unique_ptr<GlobTree> glob_tree;
  // Additional flags to pass to wildmatch in the glob_generator
  int glob_flags{0};

  std::chrono::milliseconds sync_timeout{0};
  uint32_t lock_timeout{0};

  // We can't (and mustn't!) evaluate the clockspec
  // fully until we execute query, because we have
  // to evaluate named cursors and determine fresh
  // instance at the time we execute
  std::unique_ptr<ClockSpec> since_spec;

  std::unique_ptr<QueryExpr> expr;

  // The query that we parsed into this struct
  json_ref query_spec;

  QueryFieldList fieldList;

  w_string request_id;
  w_string subscriptionName;
  pid_t clientPid{0};

  ~Query();

  /** Returns true if the supplied name is contained in
   * the parsed fieldList in this query */
  bool isFieldRequested(w_string_piece name) const;
};

} // namespace watchman
