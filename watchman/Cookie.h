/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <string_view>
#include "watchman/watchman_string.h"

namespace watchman {

inline constexpr std::string_view kCookiePrefix = ".watchman-cookie-";

/**
 * We need to guarantee that we never collapse a cookie notification
 * out of the pending list, because we absolutely must observe it coming
 * in via the kernel notification mechanism in order for synchronization
 * to be correct.
 * Since we don't have a watchman_root available, we can't tell what the
 * precise cookie prefix is for the current pending list here, so
 * we do a substring match.  Not the most elegant thing in the world.
 */
inline bool isPossiblyACookie(const w_string_t* path) {
  return w_string_contains_cstr_len(
      path, kCookiePrefix.data(), kCookiePrefix.size());
}

} // namespace watchman
