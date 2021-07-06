/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman/saved_state/SavedStateInterface.h"
#include <memory>
#include "watchman/Errors.h"
#include "watchman/saved_state/LocalSavedStateInterface.h"
#include "watchman/watchman.h"

namespace watchman {

SavedStateInterface::~SavedStateInterface() = default;

SavedStateInterface::SavedStateInterface(const json_ref& savedStateConfig) {
  auto project = savedStateConfig.get_default("project");
  if (!project) {
    throw QueryParseError("'project' must be present in saved state config");
  }
  if (!project.isString()) {
    throw QueryParseError("'project' must be a string");
  }
  project_ = json_to_w_string(project);
  auto projectMetadata = savedStateConfig.get_default("project-metadata");
  if (projectMetadata) {
    if (!projectMetadata.isString()) {
      throw QueryParseError("'project-metadata' must be a string");
    }
    projectMetadata_ = json_to_w_string(projectMetadata);
  } else {
    projectMetadata_ = w_string();
  }
}

SavedStateInterface::SavedStateResult
SavedStateInterface::getMostRecentSavedState(
    w_string_piece lookupCommitId) const {
  try {
    return getMostRecentSavedStateImpl(lookupCommitId);
  } catch (const std::exception& ex) {
    // This is a performance optimization only so return an error message on
    // failure but do not throw.
    auto reason = ex.what();
    log(ERR, "Exception while finding most recent saved state: ", reason, "\n");
    SavedStateInterface::SavedStateResult result;
    result.commitId = w_string();
    result.savedStateInfo = json_object(
        {{"error", w_string_to_json("Error while finding saved state")}});
    return result;
  }
}
} // namespace watchman
