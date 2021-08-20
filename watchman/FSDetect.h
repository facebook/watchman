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

#include <folly/Range.h>
#include "watchman/watchman_string.h"

// Returns the name of the filesystem for the specified path
w_string w_fstype(const char* path);
w_string find_fstype_in_linux_proc_mounts(
    folly::StringPiece path,
    folly::StringPiece procMountsData);

inline bool is_edenfs_fs_type(w_string_piece fs_type) {
  return fs_type == "edenfs" || fs_type.startsWith("edenfs:");
}
