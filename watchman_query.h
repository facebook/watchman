/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_QUERY_H
#define WATCHMAN_QUERY_H
#include <array>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "Future.h"
#include "watchman_clockspec.h"
#include "FileSystem.h"

namespace watchman {
  struct FileInformation;
}
struct watchman_file;

struct w_query;
typedef struct w_query w_query;

struct w_query_field_renderer {
  w_string name;
  json_ref (*make)(const struct watchman_rule_match* match);
  watchman::Future<json_ref> (*futureMake)(
      const struct watchman_rule_match* match);
};

using w_query_field_list = std::vector<const w_query_field_renderer*>;

struct w_query_since {
  bool is_timestamp;
  union {
    time_t timestamp;
    struct {
      bool is_fresh_instance;
      uint32_t ticks;
    } clock;
  };

  w_query_since() : is_timestamp(false), clock{true, 0} {}
};

// A View-independent way of accessing file properties in the
// query engine.  A FileResult is not intended to be accessed
// concurrently from multiple threads and may be unsafe to
// be used in that manner (there is no implied locking).
class FileResult {
 public:
  virtual ~FileResult();
  virtual const watchman::FileInformation& stat() const = 0;
  // Returns the name of the file in its containing dir
  virtual w_string_piece baseName() const = 0;
  // Returns the name of the containing dir relative to the
  // VFS root
  virtual w_string_piece dirName() = 0;
  // Returns true if the file currently exists
  virtual bool exists() const = 0;
  // Returns the symlink target
  virtual watchman::Future<w_string> readLink() const = 0;

  virtual const w_clock_t& ctime() const = 0;
  virtual const w_clock_t& otime() const = 0;

  // Returns the SHA-1 hash of the file contents
  using ContentHash = std::array<uint8_t, 20>;
  virtual watchman::Future<ContentHash> getContentSha1() = 0;
};

struct watchman_rule_match {
  uint32_t root_number;
  w_string relname;
  bool is_new;
  std::unique_ptr<FileResult> file;

  watchman_rule_match(
      uint32_t root_number,
      const w_string& relname,
      bool is_new,
      std::unique_ptr<FileResult>&& file)
      : root_number(root_number),
        relname(relname),
        is_new(is_new),
        file(std::move(file)) {}
};

// Holds state for the execution of a query
struct w_query_ctx {
  struct w_query *query;
  std::shared_ptr<w_root_t> root;
  std::unique_ptr<FileResult> file;
  w_string wholename;
  struct w_query_since since;
  // root number, ticks at start of query execution
  ClockSpec clockAtStartOfQuery;
  uint32_t lastAgeOutTickValueAtStartOfQuery;

  // Rendered results
  json_ref resultsArray;

  // Results that are pending render, eg: pending some
  // computation that is happening async.
  std::deque<watchman::Future<json_ref>> resultsToRender;

  // When deduping the results, set<wholename> of
  // the files held in results
  std::unordered_set<w_string> dedup;

  // How many times we suppressed a result due to dedup checking
  uint32_t num_deduped{0};

  // Disable fresh instance queries
  bool disableFreshInstance{false};

  w_query_ctx(
      w_query* q,
      const std::shared_ptr<w_root_t>& root,
      bool disableFreshInstance);
  w_query_ctx(const w_query_ctx&) = delete;
  w_query_ctx& operator=(const w_query_ctx&) = delete;

  // Move any completed items from resultsToRender to resultsArray
  void speculativeRenderCompletion();

  // Increment numWalked_ by the specified amount
  inline void bumpNumWalked(int64_t amount = 1) {
    numWalked_ += amount;
  }

  int64_t getNumWalked() const {
    return numWalked_;
  }

 private:
  // Number of files considered as part of running this query
  int64_t numWalked_{0};
};

struct w_query_path {
  w_string name;
  int depth;
};

class QueryExpr {
 public:
  virtual ~QueryExpr();
  virtual bool evaluate(w_query_ctx* ctx, const FileResult* file) = 0;
};

struct watchman_glob_tree;

// represents an error parsing a query
class QueryParseError : public std::runtime_error {
 public:
  template <typename... Args>
  explicit QueryParseError(Args&&... args)
      : std::runtime_error(watchman::to<std::string>(
            "failed to parse query: ",
            std::forward<Args>(args)...)) {}
};

// represents an error executing a query
class QueryExecError : public std::runtime_error {
 public:
  template <typename... Args>
  explicit QueryExecError(Args&&... args)
      : std::runtime_error(watchman::to<std::string>(
            "query failed: ",
            std::forward<Args>(args)...)) {}
};

struct w_query {
  watchman::CaseSensitivity case_sensitive{watchman::CaseSensitivity::CaseInSensitive};
  bool empty_on_fresh_instance{false};
  bool dedup_results{false};

  /* optional full path to relative root, without and with trailing slash */
  w_string relative_root;
  w_string relative_root_slash;

  std::vector<w_query_path> paths;

  std::unique_ptr<watchman_glob_tree> glob_tree;
  // Additional flags to pass to wildmatch in the glob_generator
  int glob_flags{0};

  std::vector<w_string> suffixes;

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

  w_query_field_list fieldList;
  // True if any entry in fieldList has a non-null futureMake
  bool renderUsesFutures{false};
};

typedef std::unique_ptr<QueryExpr> (
    *w_query_expr_parser)(w_query* query, const json_ref& term);

bool w_query_register_expression_parser(
    const char *term,
    w_query_expr_parser parser);

std::shared_ptr<w_query> w_query_parse(
    const std::shared_ptr<w_root_t>& root,
    const json_ref& query);

std::unique_ptr<QueryExpr> w_query_expr_parse(
    w_query* query,
    const json_ref& term);

bool w_query_file_matches_relative_root(
    struct w_query_ctx* ctx,
    const watchman_file* file);

// Allows a generator to process a file node
// through the query engine
void w_query_process_file(
    w_query* query,
    struct w_query_ctx* ctx,
    std::unique_ptr<FileResult> file);

// Generator callback, used to plug in an alternate
// generator when used in triggers or subscriptions
using w_query_generator = std::function<void(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    struct w_query_ctx* ctx)>;
void time_generator(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    struct w_query_ctx* ctx);

struct w_query_res {
  bool is_fresh_instance;
  json_ref resultsArray;
  // Only populated if the query was set to dedup_results
  std::unordered_set<w_string> dedupedFileNames;
  ClockSpec clockAtStartOfQuery;
};

w_query_res w_query_execute(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    w_query_generator generator);

// Returns a shared reference to the wholename
// of the file.  The caller must not delref
// the reference.
const w_string& w_query_ctx_get_wholename(struct w_query_ctx* ctx);

// parse the old style since and find queries
std::shared_ptr<w_query> w_query_parse_legacy(
    const std::shared_ptr<w_root_t>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p);
void w_query_legacy_field_list(w_query_field_list* flist);

json_ref file_result_to_json(
    const w_query_field_list& fieldList,
    const watchman_rule_match& match);

watchman::Future<json_ref> file_result_to_json_future(
    const w_query_field_list& fieldList,
    watchman_rule_match&& match);

void w_query_init_all(void);

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
bool eval_int_compare(json_int_t ival, struct w_query_int_compare *comp);

void parse_field_list(json_ref field_list, w_query_field_list* selected);
json_ref field_list_to_json_name_array(const w_query_field_list& fieldList);

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
};

#define W_TERM_PARSER1(symbol, name, func) \
  static w_ctor_fn_type(symbol) {                   \
    w_query_register_expression_parser(name, func); \
  }                                                 \
  w_ctor_fn_reg(symbol)

#define W_TERM_PARSER(name, func) \
  W_TERM_PARSER1(w_gen_symbol(w_term_register_), name, func)

#endif

/* vim:ts=2:sw=2:et:
 */
