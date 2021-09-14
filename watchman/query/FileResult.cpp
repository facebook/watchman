/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/FileResult.h"

namespace watchman {

FileResult::~FileResult() {}

folly::Optional<DType> FileResult::dtype() {
  auto statInfo = stat();
  if (!statInfo.has_value()) {
    return folly::none;
  }
  return statInfo->dtype();
}

} // namespace watchman
