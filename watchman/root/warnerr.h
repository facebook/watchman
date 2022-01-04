/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <system_error>

struct watchman_dir;

namespace watchman {

class Root;

void handle_open_errno(
    Root& root,
    watchman_dir* dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err);

} // namespace watchman
