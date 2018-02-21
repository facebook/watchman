/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "InMemoryView.h"
#include "watchman.h"
#include "watchman_error_category.h"

using namespace watchman;

namespace {
class NeedRecrawl : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
} // namespace

bool watchman_root::syncToNow(std::chrono::milliseconds timeout) {
  w_perf_t sample("sync_to_now");
  bool res;
  try {
    res = view()->syncToNow(timeout);
  } catch (const NeedRecrawl& exc) {
    scheduleRecrawl(exc.what());
    return false;
  }

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
  try {
    return cookies_.syncToNow(timeout);
  } catch (const std::system_error& exc) {
    // Note that timeouts in syncToNow are reported as a `false` return
    // value, so if we get any exception here then something must be
    // really wrong.  In practice the most likely cause is that
    // the cookie dir no longer exists.  The sanest sounding thing
    // to do in this situation is schedule a recrawl.

    if (exc.code() == watchman::error_code::no_such_file_or_directory ||
        exc.code() == watchman::error_code::not_a_directory) {
      if (cookies_.cookieDir() == root_path) {
        throw NeedRecrawl("root dir was removed and we didn't get notified");
      } else {
        // The cookie dir was a VCS subdir and it got deleted.  Let's
        // focus instead on the parent dir and recursively retry.
        cookies_.setCookieDir(root_path);
        return cookies_.syncToNow(timeout);
      }
    }

    throw;
  }
}

/* vim:ts=2:sw=2:et:
 */
