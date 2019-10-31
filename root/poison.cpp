/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void set_poison_state(
    const w_string& dir,
    struct timeval now,
    const char* syscall,
    const std::error_code& err) {
  if (!poisoned_reason->empty()) {
    return;
  }

  auto why = folly::to<std::string>(
      "A non-recoverable condition has triggered.  Watchman needs your help!\n"
      "The triggering condition was at timestamp=",
      now.tv_sec,
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
