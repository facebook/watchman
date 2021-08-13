/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

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

namespace watchman {
struct QueryContext;
}

struct watchman_file;
struct w_query;

namespace watchman {

struct QueryFieldRenderer {
  w_string name;
  folly::Optional<json_ref> (*make)(FileResult* file, const QueryContext* ctx);
};

using QueryFieldList = std::vector<QueryFieldRenderer*>;

} // namespace watchman

struct w_query_path {
  w_string name;
  int depth;
};

// Describes how terms are being aggregated
enum AggregateOp {
  AnyOf,
  AllOf,
};

using EvaluateResult = folly::Optional<bool>;

class QueryExpr {
 protected:
  using FileResult = watchman::FileResult;
  using QueryContext = watchman::QueryContext;

 public:
  virtual ~QueryExpr();
  virtual EvaluateResult evaluate(QueryContext* ctx, FileResult* file) = 0;

  // If OTHER can be aggregated with THIS, returns a new expression instance
  // representing the combined state.  Op provides information on the containing
  // query and can be used to determine how aggregation is done.
  // returns nullptr if no aggregation was performed.
  virtual std::unique_ptr<QueryExpr> aggregate(
      const QueryExpr* other,
      const AggregateOp op) const;
};

struct watchman_glob_tree;

struct w_query {
  using FileResult = watchman::FileResult;
  using QueryContext = watchman::QueryContext;
  using QueryFieldList = watchman::QueryFieldList;

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

  std::unique_ptr<watchman_glob_tree> glob_tree;
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
};

typedef std::unique_ptr<QueryExpr> (
    *w_query_expr_parser)(w_query* query, const json_ref& term);

bool w_query_register_expression_parser(
    const char* term,
    w_query_expr_parser parser);

std::shared_ptr<w_query> w_query_parse(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& query);

std::unique_ptr<QueryExpr> w_query_expr_parse(
    w_query* query,
    const json_ref& term);

// Allows a generator to process a file node
// through the query engine
void w_query_process_file(
    w_query* query,
    watchman::QueryContext* ctx,
    std::unique_ptr<watchman::FileResult> file);

// Generator callback, used to plug in an alternate
// generator when used in triggers or subscriptions
using w_query_generator = std::function<void(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryContext* ctx)>;
void time_generator(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    watchman::QueryContext* ctx);

struct w_query_res {
  bool is_fresh_instance;
  json_ref resultsArray;
  // Only populated if the query was set to dedup_results
  std::unordered_set<w_string> dedupedFileNames;
  ClockSpec clockAtStartOfQuery;
  uint32_t stateTransCountAtStartOfQuery;
  json_ref savedStateInfo;
};

w_query_res w_query_execute(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    w_query_generator generator);

// Returns a shared reference to the wholename
// of the file.  The caller must not delref
// the reference.
const w_string& w_query_ctx_get_wholename(watchman::QueryContext* ctx);

// parse the old style since and find queries
std::shared_ptr<w_query> w_query_parse_legacy(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p);
void w_query_legacy_field_list(watchman::QueryFieldList* flist);

void w_query_init_all();

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
// A node in the tree of node matching rules
struct watchman_glob_tree {
  std::string pattern;

  // The list of child rules, excluding any ** rules
  std::vector<std::unique_ptr<watchman_glob_tree>> children;
  // The list of ** rules that exist under this node
  std::vector<std::unique_ptr<watchman_glob_tree>> doublestar_children;

  unsigned is_leaf : 1; // if true, generate files for matches
  unsigned had_specials : 1; // if false, can do simple string compare
  unsigned is_doublestar : 1; // pattern begins with **

  watchman_glob_tree(const char* pattern, uint32_t pattern_len);

  // Produces a list of globs from the glob tree, effectively
  // performing the reverse of the original parsing operation.
  std::vector<std::string> unparse() const;

  // A helper method for unparse
  void unparse_into(
      std::vector<std::string>& globStrings,
      folly::StringPiece relative) const;
};

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
