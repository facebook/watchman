/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <optional>

#if HAVE_FSEVENTS

namespace watchman {

struct fse_stream;

struct watchman_fsevent {
  w_string path;
  FSEventStreamEventFlags flags;

  watchman_fsevent(w_string&& path, FSEventStreamEventFlags flags)
      : path(std::move(path)), flags(flags) {}
};

struct FSEventsWatcher : public Watcher {
  watchman::Pipe fse_pipe;

  std::condition_variable fse_cond;
  // Unflattened queue of pending events. The fse_callback function will push
  // exactly one vector to the end of this one, flattening the vector would
  // require extra copying and allocations.
  folly::Synchronized<std::vector<std::vector<watchman_fsevent>>, std::mutex>
      items_;

  struct fse_stream* stream{nullptr};
  bool attempt_resync_on_drop{false};
  bool has_file_watching{false};
  std::optional<w_string> subdir{std::nullopt};

  explicit FSEventsWatcher(
      bool hasFileWatching,
      std::optional<w_string> dir = std::nullopt);

  explicit FSEventsWatcher(
      w_root_t* root,
      std::optional<w_string> dir = std::nullopt);

  bool start(const std::shared_ptr<w_root_t>& root) override;

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      const char* path) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
  void FSEventsThread(const std::shared_ptr<w_root_t>& root);
};

} // namespace watchman

#endif
