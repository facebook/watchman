/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/watchman.h"

void set_poison_state(
    const w_string& dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err) {
  if (!poisoned_reason.rlock()->empty()) {
    return;
  }

  auto why = folly::to<std::string>(
      "A non-recoverable condition has triggered.  Watchman needs your help!\n"
      "The triggering condition was at timestamp=",
      std::chrono::system_clock::to_time_t(now),
      ": ",
      syscall,
      "(",
      dir,
      ") -> ",
      err.message(),
      "\n"
      "All requests will continue to fail with this message until you resolve\n"
      "the underlying problem.  You will find more information on fixing this at\n",
      cfg_get_trouble_url(),
      "#poison-",
      syscall,
      "\n");

  watchman::log(watchman::ERR, why);
  *poisoned_reason.wlock() = why;
}

/* vim:ts=2:sw=2:et:
 */
