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

#include "watchman/saved_state/SavedStateFactory.h"
#include "watchman/Errors.h"
#include "watchman/saved_state/LocalSavedStateInterface.h"

#if HAVE_MANIFOLD
#include "watchman/facebook/saved_state/ManifoldSavedStateInterface.h" // @manual
#endif

namespace watchman {

std::unique_ptr<SavedStateInterface> getInterface(
    w_string_piece storageType,
    const json_ref& savedStateConfig,
    const SCM* scm,
    const std::shared_ptr<watchman_root> root) {
  unused_parameter(root);
#if HAVE_MANIFOLD
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
