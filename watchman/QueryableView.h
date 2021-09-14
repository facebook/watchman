/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <future>
#include <vector>
#include "watchman/PerfSample.h"
#include "watchman/watchman_query.h"
#include "watchman/watchman_string.h"

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
  virtual void timeGenerator(w_query* query, QueryContext* ctx) const;

  /**
   * Walks files that match the supplied set of paths.
   */
  virtual void pathGenerator(w_query* query, QueryContext* ctx) const;

  virtual void globGenerator(w_query* query, QueryContext* ctx) const;

  virtual void allFilesGenerator(w_query* query, QueryContext* ctx) const;

  virtual ClockPosition getMostRecentRootNumberAndTickValue() const = 0;
  virtual w_string getCurrentClockString() const = 0;
  virtual uint32_t getLastAgeOutTickValue() const;
  virtual std::chrono::system_clock::time_point getLastAgeOutTimeStamp() const;
  virtual void ageOut(PerfSample& sample, std::chrono::seconds minAge);
  virtual void syncToNow(
      const std::shared_ptr<watchman_root>& root,
      std::chrono::milliseconds timeout,
      std::vector<w_string>& cookieFileNames) = 0;

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
