/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"

#include <string>
#include <unordered_map>
#include "ChildProcess.h"
#include "FileInformation.h"
#include "SCM.h"
#include "watchman_synchronized.h"

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
  mutable Synchronized<infoCache> cache_;
};
}
