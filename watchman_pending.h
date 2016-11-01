/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <chrono>

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
  watchman_pending_fs* pending_;

  watchman_pending_collection();
  watchman_pending_collection(const watchman_pending_collection&) = delete;
  ~watchman_pending_collection();

  void drain();
  void lock();
  void unlock();
  bool add(const w_string& path, struct timeval now, int flags);
  bool add(
      struct watchman_dir* dir,
      const char* name,
      struct timeval now,
      int flags);
  void append(watchman_pending_collection* src);
  struct watchman_pending_fs* pop();
  bool lockAndWait(std::chrono::milliseconds timeoutms);
  void ping();
  uint32_t size() const;

 private:
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  bool pinged_;
  art_tree<watchman_pending_fs*> tree_;

  struct iterContext {
    const w_string& root;
    watchman_pending_collection& coll;

    int operator()(
        const unsigned char* key,
        uint32_t key_len,
        watchman_pending_fs*& p);

    iterContext(const w_string& root, watchman_pending_collection& coll);
  };
  friend struct iterContext;

  void maybePruneObsoletedChildren(const w_string& path, int flags);
  inline void consolidateItem(watchman_pending_fs* p, int flags);
  bool isObsoletedByContainingDir(const w_string& path);
  inline void linkHead(watchman_pending_fs* p);
  inline void unlinkItem(watchman_pending_fs* p);
};

void w_pending_fs_free(watchman_pending_fs* p);
