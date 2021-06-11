/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <folly/experimental/LockFreeRingBuffer.h>
#include <optional>

#if HAVE_FSEVENTS

namespace watchman {

struct fse_stream;
struct FSEventsLogEntry;

struct watchman_fsevent {
  w_string path;
  FSEventStreamEventFlags flags;

  watchman_fsevent(w_string&& path, FSEventStreamEventFlags flags)
      : path(std::move(path)), flags(flags) {}
};

struct FSEventsWatcher : public Watcher {
  watchman::Pipe fse_pipe;

  std::condition_variable fse_cond;
  struct Items {
    // Unflattened queue of pending events. The fse_callback function will push
    // exactly one vector to the end of this one, flattening the vector would
    // require extra copying and allocations.
    std::vector<std::vector<watchman_fsevent>> items;
    // Sync requests to be inserted into PendingCollection.
    std::vector<folly::Promise<folly::Unit>> syncs;
  };
  folly::Synchronized<Items, std::mutex> items_;

  struct fse_stream* stream{nullptr};
  bool attempt_resync_on_drop{false};
  bool has_file_watching{false};
  std::optional<w_string> subdir{std::nullopt};

  // Incremented in fse_callback
  std::atomic<size_t> totalEventsSeen_{0};
  /**
   * If not null, holds a fixed-size ring of the last `fsevents_ring_log_size`
   * FSEvents events.
   */
  std::unique_ptr<folly::LockFreeRingBuffer<FSEventsLogEntry>> ringBuffer_;

  explicit FSEventsWatcher(
      bool hasFileWatching,
      std::optional<w_string> dir = std::nullopt);

  explicit FSEventsWatcher(
      watchman_root* root,
      std::optional<w_string> dir = std::nullopt);
  ~FSEventsWatcher();

  bool start(const std::shared_ptr<watchman_root>& root) override;

  folly::SemiFuture<folly::Unit> flushPendingEvents() override;

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<watchman_root>& root,
      struct watchman_dir* dir,
      const char* path) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<watchman_root>& root,
      PendingChanges& changes) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
  void FSEventsThread(const std::shared_ptr<watchman_root>& root);

  json_ref getDebugInfo() override;
};

} // namespace watchman

#endif
