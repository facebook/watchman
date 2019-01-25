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
} // namespace watchman
