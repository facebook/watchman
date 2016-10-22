/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool root_start(w_root_t* root, char**) {
  root->inner.view->startThreads(root);
  return true;
}

void w_root_schedule_recrawl(w_root_t *root, const char *why) {
  auto info = root->recrawlInfo.wlock();

  if (!info->shouldRecrawl) {
    info->lastRecrawlReason =
        w_string::printf("%s: %s", root->root_path.c_str(), why);

    w_log(
        W_LOG_ERR,
        "%s: %s: scheduling a tree recrawl\n",
        root->root_path.c_str(),
        why);
  }
  info->shouldRecrawl = true;
  signal_root_threads(root);
}

void signal_root_threads(w_root_t *root) {
  root->inner.view->signalThreads();
}

// Cancels a watch.
bool w_root_cancel(w_root_t *root /* don't care about locked state */) {
  bool cancelled = false;

  if (!root->inner.cancelled) {
    cancelled = true;

    w_log(W_LOG_DBG, "marked %s cancelled\n", root->root_path.c_str());
    root->inner.cancelled = true;

    signal_root_threads(root);
    remove_root_from_watched(root);
  }

  return cancelled;
}

bool w_root_stop_watch(struct unlocked_watchman_root *unlocked) {
  bool stopped = remove_root_from_watched(unlocked->root);

  if (stopped) {
    w_root_cancel(unlocked->root);
    w_state_save(); // this is what required that we are not locked
  }
  signal_root_threads(unlocked->root);

  return stopped;
}

/* vim:ts=2:sw=2:et:
 */
