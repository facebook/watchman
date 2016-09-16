/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void set_poison_state(w_string_t *dir, struct timeval now,
                      const char *syscall, int err, const char *reason) {
  char *why = NULL;

  if (poisoned_reason) {
    return;
  }

  ignore_result(asprintf(&why,
"A non-recoverable condition has triggered.  Watchman needs your help!\n"
"The triggering condition was at timestamp=%ld: %s(%.*s) -> %s\n"
"All requests will continue to fail with this message until you resolve\n"
"the underlying problem.  You will find more information on fixing this at\n"
"%s#poison-%s\n",
    (long)now.tv_sec,
    syscall,
    dir->len,
    dir->buf,
    reason ? reason : strerror(err),
    cfg_get_trouble_url(),
    syscall
  ));

  w_log(W_LOG_ERR, "%s", why);

  // This assignment can race for store with other threads.  We don't
  // care about that; we consider ourselves broken and the worst case
  // is that we leak a handful of strings around the race
  poisoned_reason = why;
}


/* vim:ts=2:sw=2:et:
 */
