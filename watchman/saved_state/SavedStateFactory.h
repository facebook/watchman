/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"

struct watchman_root;

namespace watchman {

class SCM;
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
