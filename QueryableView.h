/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_string.h"
#include <future>
#include <vector>

struct watchman_file;
struct watchman_dir;
struct watchman_glob_tree;

namespace watchman {

class QueryableView : public std::enable_shared_from_this<QueryableView> {
 public:
  virtual ~QueryableView();

  /** Perform a time-based (since) query and emit results to the supplied
  * query context */
  virtual bool timeGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const;

  /** Walks all files with the suffix(es) configured in the query */
  virtual bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const;

  /** Walks files that match the supplied set of paths */
  virtual bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const;

  virtual bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const;

  virtual bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const;
  virtual ClockPosition getMostRecentRootNumberAndTickValue() const = 0;
  virtual w_string getCurrentClockString() const = 0;
  virtual uint32_t getLastAgeOutTickValue() const;
  virtual time_t getLastAgeOutTimeStamp() const;
  virtual void ageOut(w_perf_t& sample, std::chrono::seconds minAge);
  virtual bool syncToNow(std::chrono::milliseconds timeout) = 0;

  // Specialized query function that is used to test whether
  // version control files exist as part of some settling handling.
  // It should query the view and return true if any of the named
  // files current exist in the view.
  virtual bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const = 0;

  bool isVCSOperationInProgress() const;

  // Start up any helper threads
  virtual void startThreads(const std::shared_ptr<w_root_t>& root);
  // Request that helper threads shutdown (but does not join them)
  virtual void signalThreads();

  virtual const w_string& getName() const = 0;

  virtual std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<w_root_t>& root) = 0;
};
}
