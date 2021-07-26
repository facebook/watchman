/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include <chrono>
#include <folly/String.h>
#include <vector>
#include "watchman/ChildProcess.h"
#include "watchman/LRUCache.h"
#include "watchman/scm/SCM.h"

namespace watchman {

class Git : public SCM {
 public:
  Git(w_string_piece rootPath, w_string_piece scmRoot);
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
  std::vector<w_string> getCommitsPriorToAndIncluding(
      w_string_piece commitId,
      int numCommits,
      w_string requestId = nullptr) const override;

 private:
  std::string indexPath_;
  mutable LRUCache<std::string, std::vector<w_string>> commitsPrior_;
  mutable LRUCache<std::string, w_string> mergeBases_;
  mutable LRUCache<std::string, w_string> filesChangedBetweenCommits_;
  mutable LRUCache<std::string, std::vector<w_string>>
      filesChangedSinceMergeBaseWith_;

  ChildProcess::Options makeGitOptions(w_string requestId) const;
  struct timespec getIndexMtime() const;
};

} // namespace watchman
