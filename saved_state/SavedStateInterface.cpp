/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "SavedStateInterface.h"
#include "LocalSavedStateInterface.h"
#include "watchman.h"

namespace watchman {

SavedStateInterface::~SavedStateInterface() {}

std::unique_ptr<SavedStateInterface> SavedStateInterface::getInterface(
    w_string_piece storageType,
    const json_ref& savedStateConfig,
    const SCM* scm) {
  if (storageType == "local") {
    return watchman::make_unique<LocalSavedStateInterface>(
        savedStateConfig, scm);
  }
  throw QueryParseError("invalid storage type '", storageType, "'");
}
} // namespace watchman
