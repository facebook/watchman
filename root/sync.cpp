/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Returns true if we observe the change within the requested
 * time, false otherwise.
 * Must be called with the root UNLOCKED.  This function
 * will acquire and release the root lock.
 */
bool watchman_root::syncToNow(std::chrono::milliseconds timeout) {
  w_perf_t sample("sync_to_now");

  auto res = cookies.syncToNow(timeout);

  // We want to know about all timeouts
  if (!res) {
    sample.force_log();
  }
  if (sample.finish()) {
    sample.add_root_meta(this);
    sample.add_meta(
        "sync_to_now",
        json_object({{"success", json_boolean(res)},
                     {"timeoutms", json_integer(timeout.count())}}));
    sample.log();
  }

  return res;
}

/* vim:ts=2:sw=2:et:
 */
