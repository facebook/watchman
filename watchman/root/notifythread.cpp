/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Constants.h"
#include "watchman/InMemoryView.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_root.h"

namespace watchman {

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
void InMemoryView::notifyThread(const std::shared_ptr<watchman_root>& root) {
  PendingChanges fromWatcher;

  if (!watcher_->start(root)) {
    logf(
        ERR,
        "failed to start root {}, cancelling watch: {}\n",
        root->root_path,
        root->failure_reason);
    root->cancel();
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  pending_.lock()->ping();

  while (!stopThreads_) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (!watcher_->waitNotify(86400)) {
      continue;
    }
    do {
      auto resultFlags = watcher_->consumeNotify(root, fromWatcher);

      if (resultFlags.cancelSelf) {
        root->cancel();
        break;
      }
      if (fromWatcher.getPendingItemCount() >= WATCHMAN_BATCH_LIMIT) {
        break;
      }
    } while (watcher_->waitNotify(0));

    if (!fromWatcher.empty()) {
      auto lock = pending_.lock();
      lock->append(fromWatcher.stealItems(), fromWatcher.stealSyncs());
      lock->ping();
    }
  }
}
} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
