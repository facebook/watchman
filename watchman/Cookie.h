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
inline bool isPossiblyACookie(w_string_piece path) {
  return path.contains(kCookiePrefix);
}

} // namespace watchman
