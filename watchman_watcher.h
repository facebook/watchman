/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <folly/futures/Future.h>
#include <stdexcept>
#include "PendingCollection.h"
#include "watchman_opendir.h"

namespace watchman {
class QueryableView;
class InMemoryView;
class TerminalWatcherError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
} // namespace watchman

class Watcher : public std::enable_shared_from_this<Watcher> {
 public:
  /**
   * This Watcher instance's name.
   */
  const w_string name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if renames do not reliably report the individual
  // files renamed in the hierarchy
#define WATCHER_COALESCED_RENAME 2
  // if the watcher only watches the directories, and not the individual files.
#define WATCHER_ONLY_DIRECTORY_NOTIFICATIONS 4
  // if the watcher is comprised of multiple watchers
#define WATCHER_HAS_SPLIT_WATCH 8
  unsigned flags;

  Watcher(const char* name, unsigned flags);

  Watcher(Watcher&&) = delete;
  Watcher& operator=(Watcher&&) = delete;

  // Start up threads or similar.  Called in the context of the
  // notify thread
  virtual bool start(const std::shared_ptr<watchman_root>& root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  virtual ~Watcher();

  /**
   * If the returned SemiFuture is valid(), then this watcher requires
   * flushing any queued events. A Promise has been placed in the
   * PendingCollection and will be completed when InMemoryView processes the
   * event.
   *
   * In particular, FSEvents may return pending events out of order, so the
   * observation of a cookie file does not guarantee all prior changes have been
   * seen.
   *
   * Otherwise, this watcher does not require flushing, and a cookie file event
   * is considered sufficient synchronization.
   */
  virtual folly::SemiFuture<folly::Unit> flushPendingEvents() {
    return folly::SemiFuture<folly::Unit>::makeEmpty();
  }

  // Initiate an OS-level watch on the provided file
  virtual bool startWatchFile(struct watchman_file* file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or NULL on error
  virtual std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<watchman_root>& root,
      struct watchman_dir* dir,
      const char* path) = 0;

  // Signal any threads to terminate.  Do not join them here.
  virtual void signalThreads();

  struct ConsumeNotifyRet {
    // Were events added to the collection?
    bool addedPending;
    // Should the watch be cancelled?
    bool cancelSelf;
  };

  /**
   * Wait for an inotify event to become available.
   * Returns true if events are available or false if signalThreads() has been
   * called or timeout has elapsed.
   */
  virtual bool waitNotify(int timeoutms) = 0;

  /**
   * Consume any available notifications.  If there are none pending,
   * does not block.
   *
   * Notifications are inserted into `coll`.
   */
  virtual ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<watchman_root>& root,
      PendingChanges& coll) = 0;

  /**
   * Returns a JSON value containing this watcher's debug state. Intended for
   * inclusion in diagnostics.
   */
  virtual json_ref getDebugInfo();
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
      std::function<std::shared_ptr<watchman::QueryableView>(watchman_root*)>
          init,
      int priority = 0);

  /** Locate the appropriate watcher for root and initialize it */
  static std::shared_ptr<watchman::QueryableView> initWatcher(
      watchman_root* root);

  const std::string& getName() const {
    return name_;
  }

 private:
  std::string name_;
  std::function<std::shared_ptr<watchman::QueryableView>(watchman_root*)> init_;
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
            [](watchman_root* root) {
              return std::make_shared<watchman::InMemoryView>(
                  root, std::make_shared<WATCHER>(root));
            },
            priority) {}
};
