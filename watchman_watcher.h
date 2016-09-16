/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_ops {
  // What's it called??
  const char *name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if renames do not reliably report the individual
  // files renamed in the hierarchy
#define WATCHER_COALESCED_RENAME 2
  unsigned flags;

  // Perform watcher-specific initialization for a watched root.
  // Do not start threads here
  bool (*root_init)(w_root_t *root, char **errmsg);

  // Start up threads or similar.  Called in the context of the
  // notify thread
  bool (*root_start)(w_root_t *root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  void (*root_dtor)(w_root_t *root);

  // Initiate an OS-level watch on the provided file
  bool (*root_start_watch_file)(struct write_locked_watchman_root *lock,
                                struct watchman_file *file);

  // Cancel an OS-level watch on the provided file
  void (*root_stop_watch_file)(struct write_locked_watchman_root *lock,
                               struct watchman_file *file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or NULL on error
  struct watchman_dir_handle *(*root_start_watch_dir)(
      struct write_locked_watchman_root *lock, struct watchman_dir *dir,
      struct timeval now, const char *path);

  // Cancel an OS-level watch on the provided dir
  void (*root_stop_watch_dir)(struct write_locked_watchman_root *lock,
                              struct watchman_dir *dir);

  // Signal any threads to terminate.  Do not join them here.
  void (*root_signal_threads)(w_root_t *root);

  // Consume any available notifications.  If there are none pending,
  // does not block.
  bool (*root_consume_notify)(w_root_t *root,
      struct watchman_pending_collection *coll);

  // Wait for an inotify event to become available
  bool (*root_wait_notify)(w_root_t *root, int timeoutms);

  // Called when freeing a file node
  void (*file_free)(struct watchman_file *file);
};

bool w_watcher_init(w_root_t *root, char **errmsg);
void watchman_watcher_init(void);

extern struct watchman_ops fsevents_watcher;
extern struct watchman_ops kqueue_watcher;
extern struct watchman_ops inotify_watcher;
extern struct watchman_ops portfs_watcher;
extern struct watchman_ops win32_watcher;
