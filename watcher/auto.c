/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

static struct watchman_ops *default_watcher_ops =
#if HAVE_FSEVENTS
   &fsevents_watcher;
#elif defined(HAVE_PORT_CREATE)
  // We prefer portfs if you have both portfs and inotify on the assumption
  // that this is an Illumos based system with both and that the native
  // mechanism will yield more correct behavior.
  // https://github.com/facebook/watchman/issues/84
  &portfs_watcher;
#elif defined(HAVE_INOTIFY_INIT)
  &inotify_watcher;
#elif defined(HAVE_KQUEUE)
  &kqueue_watcher;
#elif defined(_WIN32)
  &win32_watcher;
#else
# error you need to assign watcher_ops for this system
#endif

bool w_watcher_init(w_root_t *root, char **errmsg) {
  root->watcher_ops = default_watcher_ops;

  if (!root->watcher_ops->root_init(root, errmsg)) {
    return false;
  }

  w_log(W_LOG_ERR, "Using watcher mechanism %s\n", root->watcher_ops->name);
  return true;
}

/* vim:ts=2:sw=2:et:
 */
