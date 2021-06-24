/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/watchman.h"

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
