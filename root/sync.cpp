/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

bool watchman_root::syncToNow(std::chrono::milliseconds timeout) {
  w_perf_t sample("sync_to_now");

  auto res = view()->syncToNow(timeout);

  // We want to know about all timeouts
  if (!res) {
    sample.force_log();
  }
  if (sample.finish()) {
    sample.add_root_meta(shared_from_this());
    sample.add_meta(
        "sync_to_now",
        json_object({{"success", json_boolean(res)},
                     {"timeoutms", json_integer(timeout.count())}}));
    sample.log();
  }

  return res;
}

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Returns true if we observe the change within the requested
 * time, false otherwise.
 */
bool watchman::InMemoryView::syncToNow(std::chrono::milliseconds timeout) {
  return cookies_.syncToNow(timeout);
}

/* vim:ts=2:sw=2:et:
 */
