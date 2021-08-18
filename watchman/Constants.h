/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <stdint.h>

namespace watchman {

inline constexpr uint32_t kIoBufSize = 1024 * 1024;
inline constexpr uint32_t kBatchLimit = 16 * 1024;

} // namespace watchman

#define WATCHMAN_IO_BUF_SIZE (::watchman::kIoBufSize)
#define WATCHMAN_BATCH_LIMIT (::watchman::kBatchLimit)
