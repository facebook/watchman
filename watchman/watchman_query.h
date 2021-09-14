/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef WATCHMAN_QUERY_H
#define WATCHMAN_QUERY_H
#include <folly/Optional.h>
#include <array>
#include <deque>
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
struct QueryContext;
} // namespace watchman

struct watchman_file;
struct watchman_root;
struct w_query;

namespace watchman {

struct QueryFieldRenderer {
  w_string name;
  folly::Optional<json_ref> (*make)(FileResult* file, const QueryContext* ctx);
};

using QueryFieldList = std::vector<QueryFieldRenderer*>;

struct w_query_path {
  w_string name;
  int depth;
};

} // namespace watchman

struct w_query {
  using GlobTree = watchman::GlobTree;
  using QueryExpr = watchman::QueryExpr;
  using QueryFieldList = watchman::QueryFieldList;
  using w_query_path = watchman::w_query_path;

  watchman::CaseSensitivity case_sensitive{
      watchman::CaseSensitivity::CaseInSensitive};
  bool fail_if_no_saved_state{false};
  bool empty_on_fresh_instance{false};
  bool omit_changed_files{false};
  bool dedup_results{false};
  uint32_t bench_iterations{0};

  /* optional full path to relative root, without and with trailing slash */
  w_string relative_root;
  w_string relative_root_slash;

  folly::Optional<std::vector<w_query_path>> paths;

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

  /** Returns true if the supplied name is contained in
   * the parsed fieldList in this query */
  bool isFieldRequested(w_string_piece name) const;

  ~w_query();
};

typedef std::unique_ptr<watchman::QueryExpr> (
    *w_query_expr_parser)(w_query* query, const json_ref& term);

bool w_query_register_expression_parser(
    const char* term,
    w_query_expr_parser parser);

std::shared_ptr<w_query> w_query_parse(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& query);

std::unique_ptr<watchman::QueryExpr> w_query_expr_parse(
    w_query* query,
    const json_ref& term);

// Allows a generator to process a file node
// through the query engine
void w_query_process_file(
    w_query* query,
    watchman::QueryContext* ctx,
    std::unique_ptr<watchman::FileResult> file);

void time_generator(
    w_query* query,
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
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryContext* ctx)>;

} // namespace watchman

watchman::QueryResult w_query_execute(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryGenerator generator);

// parse the old style since and find queries
std::shared_ptr<w_query> w_query_parse_legacy(
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

void parse_suffixes(w_query* res, const json_ref& query);
void parse_globs(w_query* res, const json_ref& query);

#define W_TERM_PARSER1(symbol, name, func)          \
  static w_ctor_fn_type(symbol) {                   \
    w_query_register_expression_parser(name, func); \
  }                                                 \
  w_ctor_fn_reg(symbol)

#define W_TERM_PARSER(name, func) \
  W_TERM_PARSER1(w_gen_symbol(w_term_register_), name, func)

#endif

/* vim:ts=2:sw=2:et:
 */
