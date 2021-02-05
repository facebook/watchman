/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"

#include <folly/Synchronized.h>
#include <string>
#include <unordered_map>
#include "ChildProcess.h"
#include "FileInformation.h"
#include "LRUCache.h"
#include "SCM.h"

namespace watchman {

class StatusAccumulator {
 public:
  void add(w_string_piece status);

  SCM::StatusResult finalize() const;

 private:
  // -1 = removed
  // 0 = changed
  // 1 = added
  std::unordered_map<w_string, int> byFile_;
};

class Mercurial : public SCM {
 public:
  Mercurial(w_string_piece rootPath, w_string_piece scmRoot);
  w_string mergeBaseWith(w_string_piece commitId, w_string requestId = nullptr)
      const override;
  std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId,
      w_string requestId = nullptr) const override;
  StatusResult getFilesChangedBetweenCommits(
      std::vector<std::string> commits,
      w_string requestId = nullptr) const override;
  std::chrono::time_point<std::chrono::system_clock> getCommitDate(
      w_string_piece commitId,
      w_string requestId = nullptr) const override;
  // public for testing
  static std::chrono::time_point<std::chrono::system_clock> convertCommitDate(
      const char* commitDate);
  std::vector<w_string> getCommitsPriorToAndIncluding(
      w_string_piece commitId,
      int numCommits,
      w_string requestId = nullptr) const override;

 private:
  std::string dirStatePath_;
  mutable LRUCache<std::string, std::vector<w_string>> commitsPrior_;
  mutable LRUCache<std::string, w_string> mergeBases_;
  mutable LRUCache<std::string, w_string> filesChangedBetweenCommits_;
  mutable LRUCache<std::string, std::vector<w_string>>
      filesChangedSinceMergeBaseWith_;

  // Returns options for invoking hg
  ChildProcess::Options makeHgOptions(w_string requestId) const;
  struct timespec getDirStateMtime() const;
};

} // namespace watchman
