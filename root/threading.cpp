/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool start_detached_root_thread(w_root_t *root, char **errmsg,
    void*(*func)(void*), pthread_t *thr) {
  pthread_attr_t attr;
  int err;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  w_root_addref(root);
  err = pthread_create(thr, &attr, func, root);
  pthread_attr_destroy(&attr);

  if (err == 0) {
    return true;
  }

  ignore_result(asprintf(errmsg,
        "failed to pthread_create: %s\n", strerror(err)));
  w_root_delref_raw(root);
  return false;
}

bool root_start(w_root_t *root, char **errmsg) {
  if (!start_detached_root_thread(root, errmsg,
        run_notify_thread, &root->notify_thread)) {
    return false;
  }

  // Wait for it to signal that the watcher has been initialized
  w_pending_coll_lock_and_wait(&root->pending, -1 /* infinite */);
  w_pending_coll_unlock(&root->pending);

  if (!start_detached_root_thread(root, errmsg,
        run_io_thread, &root->io_thread)) {
    w_root_cancel(root);
    return false;
  }
  return true;
}

void w_root_schedule_recrawl(w_root_t *root, const char *why) {
  if (!root->inner.should_recrawl) {
    root->last_recrawl_reason =
        w_string::printf("%s: %s", root->root_path.c_str(), why);

    w_log(
        W_LOG_ERR,
        "%s: %s: scheduling a tree recrawl\n",
        root->root_path.c_str(),
        why);
  }
  root->inner.should_recrawl = true;
  signal_root_threads(root);
}

void signal_root_threads(w_root_t *root) {
  // Send SIGUSR1 to interrupt blocking syscalls on the
  // worker threads.  They'll self-terminate.
  if (!pthread_equal(root->notify_thread, pthread_self())) {
    pthread_kill(root->notify_thread, SIGUSR1);
  }
  w_pending_coll_ping(&root->pending);
  root->watcher_ops->root_signal_threads(root);
}

// Cancels a watch.
bool w_root_cancel(w_root_t *root /* don't care about locked state */) {
  bool cancelled = false;

  if (!root->inner.cancelled) {
    cancelled = true;

    w_log(W_LOG_DBG, "marked %s cancelled\n", root->root_path.c_str());
    root->inner.cancelled = true;

    signal_root_threads(root);
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
