/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"
#include <memory>
#include <vector>
#include "watchman_string.h"

namespace watchman {

class SCM {
 protected:
  // Construct an SCM instance for the specified rootPath on disk.
  // rootPath may be a child directory of the true SCM root.
  SCM(w_string_piece rootPath, w_string_piece scmRoot);

 public:
  virtual ~SCM();

  // Figure out an appropriate SCM implementation for the specified
  // rootPath.  Returns a managed pointer to it if successful.
  // Returns nullptr if rootPath doesn't appear to be tracked
  // by any source control systems known to watchman.
  // Will throw an exception if watchman encounters a problem
  // in setting up the SCM instance.
  static std::unique_ptr<SCM> scmForPath(w_string_piece rootPath);

  // Returns the root path provided during construction
  const w_string& getRootPath() const;

  // Returns the directory which is considered to be the root of
  // of the repository.  This may be a parent of the rootPath that
  // was used to construct this SCM instance.
  const w_string& getSCMRoot() const;

  // Compute the merge base between the working copy revision and the
  // specified commitId.  The commitId is typically something like "master".
  virtual w_string mergeBaseWith(w_string_piece commitId) const = 0;

  // Compute the set of paths that have changed in the commits
  // starting in the working copy and going back to the merge base
  // with the specified commitId.  This list also includes the
  // set of files that show as modified in the "status" output,
  // but NOT those that are ignored.
  virtual std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId) const = 0;

 private:
  w_string rootPath_;
  w_string scmRoot_;
};
}
