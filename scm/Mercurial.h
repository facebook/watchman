/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"

#include <string>
#include <unordered_map>
#include "SCM.h"
#include "watchman_synchronized.h"
#include "FileInformation.h"

namespace watchman {

class Mercurial : public SCM {
 public:
  Mercurial(w_string_piece rootPath, w_string_piece scmRoot);
  w_string mergeBaseWith(w_string_piece commitId) const override;
  std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId) const override;

 private:
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
