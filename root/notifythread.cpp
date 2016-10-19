/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void handle_should_recrawl(struct unlocked_watchman_root* unlocked) {
  {
    auto info = unlocked->root->recrawlInfo.rlock();
    if (!info->shouldRecrawl) {
      return;
    }
  }

  struct write_locked_watchman_root lock;
  w_root_lock(unlocked, "notify_thread: handle_should_recrawl", &lock);
  if (!lock.root->inner.cancelled) {
    char *errmsg;
    auto info = lock.root->recrawlInfo.wlock();
    auto root = lock.root;

    info->shouldRecrawl = false;

    // be careful, this is a bit of a switcheroo
    w_root_teardown(root);
    if (!w_root_init(root, &errmsg)) {
      w_log(
          W_LOG_ERR,
          "failed to init root %s, cancelling watch: %s\n",
          root->root_path.c_str(),
          errmsg);
      // this should cause us to exit from the notify loop
      w_root_cancel(root);
    }
    info->recrawlCount++;
    if (!root->inner.watcher->start(root)) {
      w_log(
          W_LOG_ERR,
          "failed to start root %s, cancelling watch: %s\n",
          root->root_path.c_str(),
          root->failure_reason.c_str());
      w_root_cancel(root);
    }
    w_pending_coll_ping(&root->pending);
  }

  w_root_unlock(&lock, unlocked);
}

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
static void notify_thread(struct unlocked_watchman_root *unlocked)
{
  struct watchman_pending_collection pending;
  struct watchman_pending_collection *root_pending = &unlocked->root->pending;

  if (!unlocked->root->inner.watcher->start(unlocked->root)) {
    w_log(
        W_LOG_ERR,
        "failed to start root %s, cancelling watch: %s\n",
        unlocked->root->root_path.c_str(),
        unlocked->root->failure_reason.c_str());
    w_root_cancel(unlocked->root);
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  w_pending_coll_lock(root_pending);
  root_pending->pinged = true;
  w_pending_coll_ping(root_pending);
  w_pending_coll_unlock(root_pending);

  while (!unlocked->root->inner.cancelled) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (unlocked->root->inner.watcher->waitNotify(86400)) {
      while (unlocked->root->inner.watcher->consumeNotify(
          unlocked->root, &pending)) {
        if (w_pending_coll_size(&pending) >= WATCHMAN_BATCH_LIMIT) {
          break;
        }
        if (!unlocked->root->inner.watcher->waitNotify(0)) {
          break;
        }
      }
      if (w_pending_coll_size(&pending) > 0) {
        w_pending_coll_lock(root_pending);
        w_pending_coll_append(root_pending, &pending);
        w_pending_coll_ping(root_pending);
        w_pending_coll_unlock(root_pending);
      }
    }

    handle_should_recrawl(unlocked);
  }
}

void *run_notify_thread(void *arg) {
  struct unlocked_watchman_root unlocked = {(w_root_t*)arg};

  w_set_thread_name("notify %s", unlocked.root->root_path.c_str());
  notify_thread(&unlocked);

  w_log(W_LOG_DBG, "out of loop\n");

  /* we'll remove it from watched roots if it isn't
   * already out of there */
  remove_root_from_watched(unlocked.root);

  w_root_delref(&unlocked);
  return 0;
}

/* vim:ts=2:sw=2:et:
 */
