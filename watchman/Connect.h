/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

namespace watchman {
class Stream;
}

// Create a connected unix socket or a named pipe client stream
std::unique_ptr<watchman::Stream> w_stm_connect(int timeoutms);
