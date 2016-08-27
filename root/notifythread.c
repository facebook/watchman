/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool handle_should_recrawl(struct write_locked_watchman_root *lock)
{
  w_root_t *root = lock->root;

  if (root->should_recrawl && !root->cancelled) {
    char *errmsg;
    // be careful, this is a bit of a switcheroo
    w_root_teardown(root);
    if (!w_root_init(root, &errmsg)) {
      w_log(W_LOG_ERR, "failed to init root %.*s, cancelling watch: %s\n",
          root->root_path->len, root->root_path->buf, errmsg);
      // this should cause us to exit from the notify loop
      w_root_cancel(root);
    }
    root->recrawl_count++;
    if (!root->watcher_ops->root_start(root)) {
      w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
          root->root_path->len, root->root_path->buf,
          root->failure_reason->len, root->failure_reason->buf);
      w_root_cancel(root);
    }
    w_pending_coll_ping(&root->pending);
    return true;
  }
  return false;
}

static bool wait_for_notify(w_root_t *root, int timeoutms)
{
  return root->watcher_ops->root_wait_notify(root, timeoutms);
}

static bool consume_notify(w_root_t *root,
    struct watchman_pending_collection *coll)
{
  return root->watcher_ops->root_consume_notify(root, coll);
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
  struct write_locked_watchman_root lock;

  if (!w_pending_coll_init(&pending)) {
    w_root_cancel(unlocked->root);
    return;
  }

  if (!unlocked->root->watcher_ops->root_start(unlocked->root)) {
    w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
          unlocked->root->root_path->len, unlocked->root->root_path->buf,
          unlocked->root->failure_reason->len,
          unlocked->root->failure_reason->buf);
    w_root_cancel(unlocked->root);
    w_pending_coll_destroy(&pending);
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  w_pending_coll_lock(root_pending);
  root_pending->pinged = true;
  w_pending_coll_ping(root_pending);
  w_pending_coll_unlock(root_pending);

  while (!unlocked->root->cancelled) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (wait_for_notify(unlocked->root, 86400)) {
      while (consume_notify(unlocked->root, &pending)) {
        if (w_pending_coll_size(&pending) >= WATCHMAN_BATCH_LIMIT) {
          break;
        }
        if (!wait_for_notify(unlocked->root, 0)) {
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

    w_root_lock(unlocked, "notify_thread: handle_should_recrawl", &lock);
    handle_should_recrawl(&lock);
    w_root_unlock(&lock, unlocked);
  }

  w_pending_coll_destroy(&pending);
}

void *run_notify_thread(void *arg) {
  struct unlocked_watchman_root unlocked = {arg};

  w_set_thread_name("notify %.*s", unlocked.root->root_path->len,
                    unlocked.root->root_path->buf);
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
