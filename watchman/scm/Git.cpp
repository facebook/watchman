/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "Git.h"
#include "watchman/watchman.h"

// Capability indicating support for the git SCM
W_CAP_REG("scm-git")

namespace watchman {

Git::Git(w_string_piece rootPath, w_string_piece scmRoot)
  : SCM(rootPath, scmRoot)
  {}

w_string Git::mergeBaseWith(w_string_piece commitId, w_string requestId) const {
  (void)commitId;
  (void)requestId;
  throw SCMError("not implemented");
}

std::vector<w_string> Git::getFilesChangedSinceMergeBaseWith(w_string_piece commitId, w_string requestId) const {
  (void)commitId;
  (void)requestId;
  throw SCMError("not implemented");
}

SCM::StatusResult Git::getFilesChangedBetweenCommits(std::vector<std::string> commits, w_string requestId) const {
  (void)commits;
  (void)requestId;
  throw SCMError("not implemented");
}

std::chrono::time_point<std::chrono::system_clock> Git::getCommitDate(w_string_piece commitId, w_string requestId) const {
  (void)commitId;
  (void)requestId;
  throw SCMError("not implemented");
}

std::vector<w_string> Git::getCommitsPriorToAndIncluding(w_string_piece commitId, int numCommits, w_string requestId) const {
  (void)commitId;
  (void)numCommits;
  (void)requestId;
  throw SCMError("not implemented");
}

} // namespace watchman
