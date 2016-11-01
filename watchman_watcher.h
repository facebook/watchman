/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct Watcher {
  // What's it called??
  const char *name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if renames do not reliably report the individual
  // files renamed in the hierarchy
#define WATCHER_COALESCED_RENAME 2
  unsigned flags;

  Watcher(const char* name, unsigned flags);

  // Perform watcher-specific initialization for a watched root.
  // Do not start threads here
  virtual bool initNew(w_root_t* root, char** errmsg) = 0;

  // Start up threads or similar.  Called in the context of the
  // notify thread
  virtual bool start(w_root_t* root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  virtual ~Watcher();

  // Initiate an OS-level watch on the provided file
  virtual bool startWatchFile(struct watchman_file* file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or NULL on error
  virtual struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) = 0;

  // Signal any threads to terminate.  Do not join them here.
  virtual void signalThreads();

  // Consume any available notifications.  If there are none pending,
  // does not block.
  virtual bool consumeNotify(
      w_root_t* root,
      PendingCollection::LockedPtr& coll) = 0;

  // Wait for an inotify event to become available
  virtual bool waitNotify(int timeoutms) = 0;
};

bool w_watcher_init(w_root_t *root, char **errmsg);

extern Watcher* fsevents_watcher;
extern Watcher* kqueue_watcher;
extern Watcher* inotify_watcher;
extern Watcher* portfs_watcher;
extern Watcher* win32_watcher;
