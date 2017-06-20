/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
void InMemoryView::notifyThread(const std::shared_ptr<w_root_t>& root) {
  PendingCollection pending;
  auto localLock = pending.wlock();

  if (!watcher_->start(root)) {
    w_log(
        W_LOG_ERR,
        "failed to start root %s, cancelling watch: %s\n",
        root->root_path.c_str(),
        root->failure_reason.c_str());
    root->cancel();
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  pending_.wlock()->ping();

  while (!stopThreads_) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (watcher_->waitNotify(86400)) {
      while (watcher_->consumeNotify(root, localLock)) {
        if (localLock->size() >= WATCHMAN_BATCH_LIMIT) {
          break;
        }
        if (!watcher_->waitNotify(0)) {
          break;
        }
      }
      if (localLock->size() > 0) {
        auto lock = pending_.wlock();
        lock->append(&*localLock);
        lock->ping();
      }
    }
  }
}
}

/* vim:ts=2:sw=2:et:
 */
