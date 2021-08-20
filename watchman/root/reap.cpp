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

#include "watchman/watchman_root.h"

bool watchman_root::considerReap() {
  if (idle_reap_age == 0) {
    return false;
  }

  auto now = std::chrono::steady_clock::now();

  if (now > inner.last_cmd_timestamp.load(std::memory_order_acquire) +
              std::chrono::seconds{idle_reap_age} &&
      (triggers.rlock()->empty()) && (now > inner.last_reap_timestamp) &&
      !unilateralResponses->hasSubscribers()) {
    // We haven't had any activity in a while, and there are no registered
    // triggers or subscriptions against this watch.
    watchman::log(
        watchman::ERR,
        "root ",
        root_path,
        " has had no activity in ",
        idle_reap_age,
        " seconds and has no triggers or subscriptions, cancelling watch.  "
        "Set idle_reap_age_seconds in your .watchmanconfig to control this "
        "behavior\n");
    return true;
  }

  inner.last_reap_timestamp = now;

  return false;
}

/* vim:ts=2:sw=2:et:
 */
