/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_string.h"
#include <vector>

struct watchman_file;
struct watchman_dir;
struct Watcher;
struct watchman_glob_tree;

namespace watchman {
class QueryableView : public std::enable_shared_from_this<QueryableView> {
 public:
  std::shared_ptr<Watcher> watcher;
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
  virtual uint32_t getMostRecentTickValue() const;
  virtual uint32_t getLastAgeOutTickValue() const;
  virtual time_t getLastAgeOutTimeStamp() const;
  virtual void ageOut(w_perf_t& sample, std::chrono::seconds minAge);

  // Specialized query function that is used to test whether
  // version control files exist as part of some settling handling.
  // It should query the view and return true if any of the named
  // files current exist in the view.
  virtual bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const = 0;

  bool isVCSOperationInProgress() const;

  // Start up any helper threads
  virtual void startThreads(w_root_t* root);
  // Request that helper threads shutdown (but does not join them)
  virtual void signalThreads();
};
}
