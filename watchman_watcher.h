/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_opendir.h"

namespace watchman {
class QueryableView;
struct InMemoryView;
}

struct Watcher : public std::enable_shared_from_this<Watcher> {
  // What's it called??
  const w_string name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if renames do not reliably report the individual
  // files renamed in the hierarchy
#define WATCHER_COALESCED_RENAME 2
  unsigned flags;

  Watcher(const char* name, unsigned flags);

  // Start up threads or similar.  Called in the context of the
  // notify thread
  virtual bool start(const std::shared_ptr<w_root_t>& root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  virtual ~Watcher();

  // Initiate an OS-level watch on the provided file
  virtual bool startWatchFile(struct watchman_file* file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or NULL on error
  virtual std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) = 0;

  // Signal any threads to terminate.  Do not join them here.
  virtual void signalThreads();

  // Consume any available notifications.  If there are none pending,
  // does not block.
  virtual bool consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) = 0;

  // Wait for an inotify event to become available
  virtual bool waitNotify(int timeoutms) = 0;
};

/** Maintains the list of available watchers.
 * This is fundamentally a map of name -> factory function.
 * Some watchers (kqueue, inotify) are available on multiple operating
 * systems: kqueue on OSX and *BSD, inotify on Linux and Solaris.
 * There are cases where a given watcher is not the preferred mechanism
 * (eg: inotify is implemented in terms of portfs on Solaris, so we
 * prefer to target the portfs layer directly), so we have a concept
 * of priority associated with the watcher.
 * Larger numbers are higher priority and will be favored when performing
 * auto-detection.
 **/
class WatcherRegistry {
 public:
  WatcherRegistry(
      const std::string& name,
      std::function<std::shared_ptr<watchman::QueryableView>(w_root_t*)> init,
      int priority = 0);

  /** Locate the appropriate watcher for root and initialize it */
  static std::shared_ptr<watchman::QueryableView> initWatcher(w_root_t* root);

  const std::string& getName() const {
    return name_;
  }

 private:
  std::string name_;
  std::function<std::shared_ptr<watchman::QueryableView>(w_root_t*)> init_;
  int pri_;

  static std::unordered_map<std::string, WatcherRegistry>& getRegistry();
  static void registerFactory(const WatcherRegistry& factory);
  static const WatcherRegistry* getWatcherByName(const std::string& name);
};

/** This template makes it less verbose for the common case of defining
 * a name -> class mapping in the registry. */
template <class WATCHER>
class RegisterWatcher : public WatcherRegistry {
 public:
  explicit RegisterWatcher(const std::string& name, int priority = 0)
      : WatcherRegistry(
            name,
            [](w_root_t* root) {
              return std::make_shared<watchman::InMemoryView>(
                  root, std::make_shared<WATCHER>(root));
            },
            priority) {}
};
