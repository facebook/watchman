/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_QUERY_H
#define WATCHMAN_QUERY_H
#include <folly/Optional.h>
#include <folly/stop_watch.h>
#include <array>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "watchman/Clock.h"
#include "watchman/FileSystem.h"

namespace watchman {
struct FileInformation;
}
struct watchman_file;

struct w_query;
struct w_query_ctx;
class FileResult;

struct w_query_field_renderer {
  w_string name;
  folly::Optional<json_ref> (*make)(FileResult* file, const w_query_ctx* ctx);
};

using w_query_field_list = std::vector<const w_query_field_renderer*>;
using watchman_root = struct watchman_root;

using w_query_since = watchman::QuerySince;

// A View-independent way of accessing file properties in the
// query engine.  A FileResult is not intended to be accessed
// concurrently from multiple threads and may be unsafe to
// be used in that manner (there is no implied locking).
class FileResult {
 public:
  virtual ~FileResult();

  // Maybe returns the file information.
  // Returns folly::none if the file information is not yet known.
  virtual folly::Optional<watchman::FileInformation> stat() = 0;

  // Returns the stat.st_atime field
  virtual folly::Optional<struct timespec> accessedTime() = 0;

  // Returns the stat.st_mtime field
  virtual folly::Optional<struct timespec> modifiedTime() = 0;

  // Returns the stat.st_ctime field
  virtual folly::Optional<struct timespec> changedTime() = 0;

  // Returns the size of the file in bytes, as reported in
  // the stat.st_size field.
  virtual folly::Optional<size_t> size() = 0;

  // Returns the name of the file in its containing dir
  virtual w_string_piece baseName() = 0;
  // Returns the name of the containing dir relative to the
  // VFS root
  virtual w_string_piece dirName() = 0;

  // Maybe return the file existence status.
  // Returns folly::none if the information is not currently known.
  virtual folly::Optional<bool> exists() = 0;

  // Returns the symlink target
  virtual folly::Optional<w_string> readLink() = 0;

  // Maybe return the change time.
  // Returns folly::none if ctime is not currently known
  virtual folly::Optional<w_clock_t> ctime() = 0;

  // Maybe return the observed time.
  // Returns folly::none if otime is not currently known
  virtual folly::Optional<w_clock_t> otime() = 0;

  // Returns the SHA-1 hash of the file contents
  using ContentHash = std::array<uint8_t, 20>;
  virtual folly::Optional<ContentHash> getContentSha1() = 0;

  // Maybe return the dtype.
  // Returns folly::none if the dtype is not currently known.
  // Returns DType::Unknown if we have dtype data but it doesn't
  // tell us the dtype (this is common on some older filesystems
  // on linux).
  virtual folly::Optional<watchman::DType> dtype();

  // A bitset of Property values
  using Properties = uint_least16_t;

  // Represents one of the FileResult fields.
  // Values are such that these can be bitwise OR'd to
  // produce a value of type `Properties` representing
  // multiple properties
  enum Property : Properties {
    // No specific fields required
    None = 0,
    // The dirName() and/or baseName() methods will be called
    Name = 1 << 0,
    // Need the mtime/ctime data returned by stat(2).
    StatTimeStamps = 1 << 1,
    // Need only enough information to distinguish between
    // file types, not the full mode information.
    FileDType = 1 << 2,
    // The ctime() method will be called
    CTime = 1 << 3,
    // The otime() method will be called
    OTime = 1 << 4,
    // The getContentSha1() method will be called
    ContentSha1 = 1 << 5,
    // The exists() method will be called
    Exists = 1 << 6,
    // Will need size information.
    Size = 1 << 7,
    // the readLink() method will be called
    SymlinkTarget = 1 << 8,
    // Need full stat metadata
    FullFileInformation = 1 << 9,
  };

  // Perform a batch fetch to fill in some missing data.
  // `files` is the set of FileResult instances that need more
  // data; their individual neededProperties_ values describes
  // the set of data that is needed.
  // `files` are assumed to all be of the same FileResult descendant,
  // and this is guaranteed by the current implementation.
  // When batchFetchProperties is called, it is invoked on one of
  // the elements of `files`.
  // The expectation is that the implementation of `batchFetchProperties`
  // will perform whatever actions are necessary to ensure that
  // a subsequent attempt to evaluate `neededProperties_` against each
  // member of `files` will not result in adding any of
  // those `FileResult` instances in being added to a deferred
  // batch.
  // The implementation of batchFetchProperties must clear
  // neededProperties_ to None.
  virtual void batchFetchProperties(
      const std::vector<std::unique_ptr<FileResult>>& files) = 0;

 protected:
  // To be called by one of the FileResult accessors when it needs
  // to record which properties are required to satisfy the request.
  void accessorNeedsProperties(Properties properties) {
    neededProperties_ |= properties;
  }

  // Clear any recorded needed properties
  void clearNeededProperties() {
    neededProperties_ = Property::None;
  }

  // Return the set of needed properties
  Properties neededProperties() const {
    return neededProperties_;
  }

 private:
  // The implementation of FileResult will set appropriate
  // bits in neededProperties_ when its accessors are called
  // and the associated data is not available.
  Properties neededProperties_{Property::None};
};

enum class QueryContextState {
  NotStarted,
  WaitingForCookieSync,
  WaitingForViewLock,
  Generating,
  Rendering,
  Completed,
};

// Holds state for the execution of a query
struct w_query_ctx {
  std::chrono::time_point<std::chrono::steady_clock> created;
  folly::stop_watch<std::chrono::milliseconds> stopWatch;
  std::atomic<QueryContextState> state{QueryContextState::NotStarted};
  std::atomic<std::chrono::milliseconds> cookieSyncDuration{
      std::chrono::milliseconds(0)};
  std::atomic<std::chrono::milliseconds> viewLockWaitDuration{
      std::chrono::milliseconds(0)};
  std::atomic<std::chrono::milliseconds> generationDuration{
      std::chrono::milliseconds(0)};
  std::atomic<std::chrono::milliseconds> renderDuration{
      std::chrono::milliseconds(0)};

  void generationStarted() {
    viewLockWaitDuration = stopWatch.lap();
    state = QueryContextState::Generating;
  }

  struct w_query* query;
  std::shared_ptr<watchman_root> root;
  std::unique_ptr<FileResult> file;
  w_string wholename;
  w_query_since since;
  // root number, ticks at start of query execution
  ClockSpec clockAtStartOfQuery;
  uint32_t lastAgeOutTickValueAtStartOfQuery;

  // Rendered results
  json_ref resultsArray;

  // When deduping the results, set<wholename> of
  // the files held in results
  std::unordered_set<w_string> dedup;

  // When unconditional_log_if_results_contain_file_prefixes is set
  // and one of those prefixes matches a file in the generated results,
  // that name is added here with the intent that this is passed
  // to the perf logger
  std::vector<w_string> namesToLog;

  // How many times we suppressed a result due to dedup checking
  uint32_t num_deduped{0};

  // Disable fresh instance queries
  bool disableFreshInstance{false};

  w_query_ctx(
      w_query* q,
      const std::shared_ptr<watchman_root>& root,
      bool disableFreshInstance);
  w_query_ctx(const w_query_ctx&) = delete;
  w_query_ctx& operator=(const w_query_ctx&) = delete;

  // Increment numWalked_ by the specified amount
  inline void bumpNumWalked(int64_t amount = 1) {
    numWalked_ += amount;
  }

  int64_t getNumWalked() const {
    return numWalked_;
  }

  // Adds `file` to the currently accumulating batch of files
  // that require data to be loaded.
  // If the batch is large enough, this will trigger `fetchEvalBatchNow()`.
  // This is intended to be called for files that still having
  // their expression cause evaluated during w_query_process_file().
  void addToEvalBatch(std::unique_ptr<FileResult>&& file);

  // Perform an immediate fetch of data for the items in the
  // evalBatch_ set, and then re-evaluate each of them by passing
  // them to w_query_process_file().
  void fetchEvalBatchNow();

  void maybeRender(std::unique_ptr<FileResult>&& file);
  void addToRenderBatch(std::unique_ptr<FileResult>&& file);

  // Perform a batch load of the items in the render batch,
  // and attempt to render those items again.
  // Returns true if the render batch is empty after rendering
  // the items, false if still more data is needed.
  bool fetchRenderBatchNow();

  w_string computeWholeName(FileResult* file) const;

  // Returns true if the filename associated with `f` matches
  // the relative_root constraint set on the query.
  // Delegates to dirMatchesRelativeRoot().
  bool fileMatchesRelativeRoot(const watchman_file* f);

  // Returns true if the path to the specified file matches the
  // relative_root constraint set on the query.  fullFilePath is
  // a fully qualified absolute path to the file.
  // Delegates to dirMatchesRelativeRoot.
  bool fileMatchesRelativeRoot(w_string_piece fullFilePath);

  // Returns true if the directory path matches the relative_root
  // constraint set on the query.  fullDirectoryPath is a fully
  // qualified absolute path to a directory.
  // If relative_root is not set, always returns true.
  bool dirMatchesRelativeRoot(w_string_piece fullDirectoryPath);

 private:
  // Number of files considered as part of running this query
  int64_t numWalked_{0};

  // Files for which we encountered NeedMoreData and that we
  // will re-evaluate once we have enough of them accumulated
  // to batch fetch the required data
  std::vector<std::unique_ptr<FileResult>> evalBatch_;

  // Similar to needBatchFetch_ above, except that the files
  // in this batch have been successfully matched by the
  // expression and are just pending data to be loaded
  // for rendering the result fields.
  std::vector<std::unique_ptr<FileResult>> renderBatch_;
};

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
 public:
  virtual ~QueryExpr();
  virtual EvaluateResult evaluate(w_query_ctx* ctx, FileResult* file) = 0;

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

  w_query_field_list fieldList;

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
    struct w_query_ctx* ctx,
    std::unique_ptr<FileResult> file);

// Generator callback, used to plug in an alternate
// generator when used in triggers or subscriptions
using w_query_generator = std::function<void(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    struct w_query_ctx* ctx)>;
void time_generator(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    struct w_query_ctx* ctx);

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
const w_string& w_query_ctx_get_wholename(struct w_query_ctx* ctx);

// parse the old style since and find queries
std::shared_ptr<w_query> w_query_parse_legacy(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p);
void w_query_legacy_field_list(w_query_field_list* flist);

folly::Optional<json_ref> file_result_to_json(
    const w_query_field_list& fieldList,
    const std::unique_ptr<FileResult>& file,
    const w_query_ctx* ctx);

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

void parse_field_list(json_ref field_list, w_query_field_list* selected);
json_ref field_list_to_json_name_array(const w_query_field_list& fieldList);

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
