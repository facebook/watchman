/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include "SavedStateInterface.h"

namespace watchman {

// Identifies the most recent saved state for a given commit from saved states
// stored on the local filesystem. The local storage path must contain a
// subdirectory for the project, and within the project directory the saved
// state for a given commit must be in a file whose name is the source control
// commit hash.
//
// Checks the most recent n commits to find a saved state, if available. If a
// saved state is not available, returns an error message in the saved state
// info JSON. If a saved state is available, returns the local path for the
// state in the saved state info JSON, along with the saved state commit id.
class LocalSavedStateInterface : public SavedStateInterface {
 public:
  LocalSavedStateInterface(const json_ref& savedStateConfig, const SCM* scm);

  SavedStateInterface::SavedStateResult getMostRecentSavedStateImpl(
      w_string_piece lookupCommitId) const override;

 private:
  json_int_t maxCommits_;
  w_string localStoragePath_;
  w_string project_;
  const SCM* scm_;
};
} // namespace watchman
