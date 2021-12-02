/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/String.h>
#include <chrono>
#include <vector>
#include "watchman/ChildProcess.h"
#include "watchman/LRUCache.h"
#include "watchman/scm/SCM.h"

namespace watchman {

class GitStatusAccumulator {
 public:
  void add(w_string_piece status);

  SCM::StatusResult finalize() const;

 private:
  // -1 = removed
  // 0 = changed
  // 1 = added
  std::unordered_map<w_string, int> byFile_;
};

class Git : public SCM {
 public:
  Git(w_string_piece rootPath, w_string_piece scmRoot);
  w_string mergeBaseWith(w_string_piece commitId, w_string requestId = nullptr)
      const override;
  std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId,
      w_string requestId = nullptr) const override;

  // Note: does not support includeDirectories. git does not report directory
  // changes, though it does introduce them when the first file is introduced
  // inside them and does prune them when the last file is deleted.
  StatusResult getFilesChangedBetweenCommits(
      std::vector<std::string> commits,
      w_string requestId,
      bool includeDirectories) const override;
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
