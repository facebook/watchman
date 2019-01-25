/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"

#include <folly/Synchronized.h>
#include <string>
#include <unordered_map>
#include "ChildProcess.h"
#include "FileInformation.h"
#include "SCM.h"

namespace watchman {

class Mercurial : public SCM {
 public:
  Mercurial(w_string_piece rootPath, w_string_piece scmRoot);
  w_string mergeBaseWith(w_string_piece commitId, w_string requestId = nullptr)
      const override;
  std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId,
      w_string requestId = nullptr) const override;
  SCM::StatusResult getFilesChangedBetweenCommits(
      w_string_piece commitA,
      w_string_piece commitB,
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
  // Returns options for invoking hg
  ChildProcess::Options makeHgOptions(w_string requestId) const;

  struct infoCache {
    std::string dirStatePath;
    FileInformation dirstate;
    std::unordered_map<std::string, w_string> mergeBases;

    explicit infoCache(std::string path);
    bool dotChanged();

    w_string lookupMergeBase(const std::string& commitId);
  };
  mutable folly::Synchronized<infoCache> cache_;
};
}
