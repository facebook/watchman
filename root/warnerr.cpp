/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// POSIX says open with O_NOFOLLOW should set errno to ELOOP if the path is a
// symlink. However, FreeBSD (which ironically originated O_NOFOLLOW) sets it to
// EMLINK.
#ifdef __FreeBSD__
#define ENOFOLLOWSYMLINK EMLINK
#else
#define ENOFOLLOWSYMLINK ELOOP
#endif

void handle_open_errno(struct write_locked_watchman_root *lock,
                       struct watchman_dir *dir, struct timeval now,
                       const char *syscall, int err, const char *reason) {
  w_string_t *dir_name = w_dir_copy_full_path(dir);
  w_string_t *warn = NULL;
  bool log_warning = true;
  bool transient = false;

  if (err == ENOENT || err == ENOTDIR || err == ENOFOLLOWSYMLINK) {
    log_warning = false;
    transient = false;
  } else if (err == EACCES || err == EPERM) {
    log_warning = true;
    transient = false;
  } else if (err == ENFILE || err == EMFILE) {
    set_poison_state(dir_name, now, syscall, err, strerror(err));
    w_string_delref(dir_name);
    return;
  } else {
    log_warning = true;
    transient = true;
  }

  if (w_string_equal(dir_name, lock->root->root_path)) {
    if (!transient) {
      w_log(W_LOG_ERR,
            "%s(%.*s) -> %s. Root was deleted; cancelling watch\n",
            syscall, dir_name->len, dir_name->buf,
            reason ? reason : strerror(err));
      w_root_cancel(lock->root);
      w_string_delref(dir_name);
      return;
    }
  }

  warn = w_string_make_printf(
      "%s(%.*s) -> %s. Marking this portion of the tree deleted",
      syscall, dir_name->len, dir_name->buf,
      reason ? reason : strerror(err));

  w_log(err == ENOENT ? W_LOG_DBG : W_LOG_ERR, "%.*s\n", warn->len, warn->buf);
  if (log_warning) {
    w_root_set_warning(lock, warn);
  }
  w_string_delref(warn);

  stop_watching_dir(lock, dir);
  w_root_mark_deleted(lock, dir, now, true);
  w_string_delref(dir_name);
}

void w_root_set_warning(struct write_locked_watchman_root *lock,
                        w_string_t *str) {
  if (lock->root->warning) {
    w_string_delref(lock->root->warning);
  }
  lock->root->warning = str;
  if (lock->root->warning) {
    w_string_addref(lock->root->warning);
  }
}


/* vim:ts=2:sw=2:et:
 */
