/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "watchman/Clock.h"
#include "watchman/FileSystem.h"
#include "watchman/query/FileResult.h"
#include "watchman/query/QueryExpr.h"

namespace watchman {
struct GlobTree;
struct Query;
struct QueryContext;
struct QueryFieldRenderer;
using QueryFieldList = std::vector<QueryFieldRenderer*>;
} // namespace watchman

struct watchman_file;
struct watchman_root;

std::shared_ptr<watchman::Query> w_query_parse(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& query);

std::unique_ptr<watchman::QueryExpr> w_query_expr_parse(
    watchman::Query* query,
    const json_ref& term);

// Allows a generator to process a file node
// through the query engine
void w_query_process_file(
    watchman::Query* query,
    watchman::QueryContext* ctx,
    std::unique_ptr<watchman::FileResult> file);

void time_generator(
    watchman::Query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryContext* ctx);

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

// Generator callback, used to plug in an alternate
// generator when used in triggers or subscriptions
using QueryGenerator = std::function<void(
    watchman::Query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryContext* ctx)>;

} // namespace watchman

watchman::QueryResult w_query_execute(
    watchman::Query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryGenerator generator);

// parse the old style since and find queries
std::shared_ptr<watchman::Query> w_query_parse_legacy(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p);
void w_query_legacy_field_list(watchman::QueryFieldList* flist);

enum w_query_icmp_op {
  W_QUERY_ICMP_EQ,
  W_QUERY_ICMP_NE,
  W_QUERY_ICMP_GT,
  W_QUERY_ICMP_GE,
  W_QUERY_ICMP_LT,
  W_QUERY_ICMP_LE,
};
struct w_query_int_compare {
  enum w_query_icmp_op op;
  json_int_t operand;
};
void parse_int_compare(const json_ref& term, struct w_query_int_compare* comp);
bool eval_int_compare(json_int_t ival, struct w_query_int_compare* comp);

void parse_field_list(json_ref field_list, watchman::QueryFieldList* selected);
json_ref field_list_to_json_name_array(
    const watchman::QueryFieldList& fieldList);

void parse_suffixes(watchman::Query* res, const json_ref& query);
void parse_globs(watchman::Query* res, const json_ref& query);

/* vim:ts=2:sw=2:et:
 */
