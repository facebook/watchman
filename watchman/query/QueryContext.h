/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <folly/stop_watch.h>
#include <unordered_set>
#include "watchman/Clock.h"

struct w_query;
struct watchman_file;
struct watchman_root;

namespace watchman {

class FileResult;

enum class QueryContextState {
  NotStarted,
  WaitingForCookieSync,
  WaitingForViewLock,
  Generating,
  Rendering,
  Completed,
};

// Holds state for the execution of a query
struct QueryContext {
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

  w_query* query;
  std::shared_ptr<watchman_root> root;
  std::unique_ptr<FileResult> file;
  w_string wholename;
  QuerySince since;
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

  QueryContext(
      w_query* q,
      const std::shared_ptr<watchman_root>& root,
      bool disableFreshInstance);
  QueryContext(const QueryContext&) = delete;
  QueryContext& operator=(const QueryContext&) = delete;

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

} // namespace watchman
