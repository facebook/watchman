/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include <future>
#include <vector>
#include "watchman/watchman_perf.h"
#include "watchman/watchman_string.h"
#include "watchman_query.h"

struct watchman_file;
struct watchman_dir;
struct watchman_glob_tree;

namespace watchman {

class SCM;

class QueryableView : public std::enable_shared_from_this<QueryableView> {
 public:
  virtual ~QueryableView();

  /**
   * Perform a time-based (since) query and emit results to the supplied
   * query context.
   */
  virtual void timeGenerator(w_query* query, struct w_query_ctx* ctx) const;

  /**
   * Walks files that match the supplied set of paths.
   */
  virtual void pathGenerator(w_query* query, struct w_query_ctx* ctx) const;

  virtual void globGenerator(w_query* query, struct w_query_ctx* ctx) const;

  virtual void allFilesGenerator(w_query* query, struct w_query_ctx* ctx) const;

  virtual ClockPosition getMostRecentRootNumberAndTickValue() const = 0;
  virtual w_string getCurrentClockString() const = 0;
  virtual uint32_t getLastAgeOutTickValue() const;
  virtual std::chrono::system_clock::time_point getLastAgeOutTimeStamp() const;
  virtual void ageOut(w_perf_t& sample, std::chrono::seconds minAge);
  virtual void syncToNow(
      const std::shared_ptr<watchman_root>& root,
      std::chrono::milliseconds timeout) = 0;

  // Specialized query function that is used to test whether
  // version control files exist as part of some settling handling.
  // It should query the view and return true if any of the named
  // files current exist in the view.
  virtual bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const = 0;

  bool isVCSOperationInProgress() const;

  /**
   * Start up any helper threads.
   */
  virtual void startThreads(const std::shared_ptr<watchman_root>& /*root*/) {}
  /**
   * Request that helper threads shutdown (but does not join them).
   */
  virtual void signalThreads() {}
  /**
   * Request that helper threads wake up and re-evaluate their state.
   */
  virtual void wakeThreads() {}

  virtual const w_string& getName() const = 0;
  virtual json_ref getWatcherDebugInfo() const = 0;
  virtual void clearWatcherDebugInfo() = 0;
  virtual std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<watchman_root>& root) = 0;

  // Return the SCM detected for this watched root
  virtual SCM* getSCM() const = 0;
};
} // namespace watchman
