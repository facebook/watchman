/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_synchronized.h"
#define WATCHMAN_COOKIE_PREFIX ".watchman-cookie-"

namespace watchman {

class CookieSync {
 public:
  explicit CookieSync(const w_string& dir);
  void setCookieDir(const w_string& dir);

  /* Ensure that we're synchronized with the state of the
   * filesystem at the current time.
   * We do this by touching a cookie file and waiting to
   * observe it via inotify.  When we see it we know that
   * we've seen everything up to the point in time at which
   * we're asking questions.
   * Returns true if we observe the change within the requested
   * time, false otherwise. */
  bool syncToNow(std::chrono::milliseconds timeout);

  /* If path is a valid cookie in the map, notify the waiter.
   * Returns true if the path matches the cookie prefix (not just
   * whether the cookie is currently valid).
   * Returns false if the path does not match our cookie prefix.
   */
  void notifyCookie(const w_string& path) const;

  // We need to guarantee that we never collapse a cookie notification
  // out of the pending list, because we absolutely must observe it coming
  // in via the kernel notification mechanism in order for synchronization
  // to be correct.
  // Since we don't have a w_root_t available, we can't tell what the
  // precise cookie prefix is for the current pending list here, so
  // we do a substring match.  Not the most elegant thing in the world.
  static inline bool isPossiblyACookie(const w_string_t* path) {
    return w_string_contains_cstr_len(
        path, WATCHMAN_COOKIE_PREFIX, sizeof(WATCHMAN_COOKIE_PREFIX) - 1);
  }

  const w_string& cookiePrefix() const {
    return cookiePrefix_;
  }

  const w_string& cookieDir() const {
    return cookieDir_;
  }

 private:
  struct Cookie {
    std::condition_variable cond;
    std::mutex mutex;
    bool seen{false};
  };

  // path to the query cookie dir
  w_string cookieDir_;
  // valid filename prefix for cookies we create
  w_string cookiePrefix_;
  // Serial number for cookie filename
  std::atomic<uint32_t> serial_{0};
  Synchronized<std::unordered_map<w_string, Cookie*>> cookies_;
};
}
