/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"

class SCM;
struct watchman_root;

namespace watchman {

class SavedStateInterface;

/**
 * Returns an appropriate SavedStateInterface implementation for the
 * specified storage type. Returns a managed pointer to the saved state
 * interface if successful. Throws if the storage type is not recognized, or
 * if the saved state interface does not successfully parse the saved state
 * config.
 */
std::unique_ptr<SavedStateInterface> getInterface(
    w_string_piece storageType,
    const json_ref& savedStateConfig,
    const SCM* scm,
    const std::shared_ptr<watchman_root> root);

} // namespace watchman
