/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "SavedStateInterface.h"
#include "watchman.h"

namespace watchman {

SavedStateInterface::~SavedStateInterface() {}

std::unique_ptr<SavedStateInterface> SavedStateInterface::getInterface(
    w_string_piece storageType) {
  throw QueryParseError("invalid storage type '", storageType, "'");
}
} // namespace watchman
