/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "SavedStateInterface.h"
#include <memory>
#include "LocalSavedStateInterface.h"
#ifdef WATCHMAN_FACEBOOK_INTERNAL
#include "facebook/saved_state/ManifoldSavedStateInterface.h"
#endif

namespace watchman {

SavedStateInterface::~SavedStateInterface() {}

std::unique_ptr<SavedStateInterface> SavedStateInterface::getInterface(
    w_string_piece storageType,
    const json_ref& savedStateConfig,
    const SCM* scm,
    const std::shared_ptr<w_root_t> root) {
  unused_parameter(root);
#ifdef WATCHMAN_FACEBOOK_INTERNAL
  if (storageType == "manifold") {
    return std::make_unique<ManifoldSavedStateInterface>(
        savedStateConfig, scm, root);
  }
#endif
  if (storageType == "local") {
    return std::make_unique<LocalSavedStateInterface>(savedStateConfig, scm);
  }
  throw QueryParseError("invalid storage type '", storageType, "'");
}

SavedStateInterface::SavedStateInterface(const json_ref& savedStateConfig) {
  auto project = savedStateConfig.get_default("project");
  if (!project) {
    throw QueryParseError("'project' must be present in saved state config");
  }
  if (!project.isString()) {
    throw QueryParseError("'project' must be a string");
  }
  project_ = json_to_w_string(project);
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
