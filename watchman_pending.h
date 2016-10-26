/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#define W_PENDING_RECURSIVE 1
#define W_PENDING_VIA_NOTIFY 2
#define W_PENDING_CRAWL_ONLY 4

struct watchman_pending_fs {
  struct watchman_pending_fs *next{nullptr}, *prev{nullptr};
  w_string path;
  struct timeval now;
  int flags;

  watchman_pending_fs(
      const w_string& path,
      const struct timeval& now,
      int flags);
};

struct watchman_pending_collection {
  struct watchman_pending_fs *pending;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  bool pinged;
  art_tree tree;

  watchman_pending_collection();
  watchman_pending_collection(const watchman_pending_collection&) = delete;
  ~watchman_pending_collection();
};

void w_pending_coll_drain(struct watchman_pending_collection *coll);
void w_pending_coll_lock(struct watchman_pending_collection *coll);
void w_pending_coll_unlock(struct watchman_pending_collection *coll);
bool w_pending_coll_add(
    struct watchman_pending_collection* coll,
    const w_string& path,
    struct timeval now,
    int flags);
bool w_pending_coll_add_rel(struct watchman_pending_collection *coll,
    struct watchman_dir *dir, const char *name,
    struct timeval now, int flags);
void w_pending_coll_append(struct watchman_pending_collection *target,
    struct watchman_pending_collection *src);
struct watchman_pending_fs *w_pending_coll_pop(
    struct watchman_pending_collection *coll);
bool w_pending_coll_lock_and_wait(struct watchman_pending_collection *coll,
    int timeoutms);
void w_pending_coll_ping(struct watchman_pending_collection *coll);
uint32_t w_pending_coll_size(struct watchman_pending_collection *coll);
void w_pending_fs_free(struct watchman_pending_fs *p);
