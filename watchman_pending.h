/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <chrono>
#include <condition_variable>
#include "watchman_synchronized.h"

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

struct PendingCollectionBase {
  watchman_pending_fs* pending_;

  PendingCollectionBase(
      std::condition_variable& cond,
      std::atomic<bool>& pinged);
  PendingCollectionBase(const PendingCollectionBase&) = delete;
  PendingCollectionBase(PendingCollectionBase&&) = default;
  ~PendingCollectionBase();

  void drain();
  bool add(const w_string& path, struct timeval now, int flags);
  bool add(
      struct watchman_dir* dir,
      const char* name,
      struct timeval now,
      int flags);
  void append(PendingCollectionBase* src);
  struct watchman_pending_fs* pop();
  uint32_t size() const;
  void ping();
  bool checkAndResetPinged();

 private:
  std::condition_variable& cond_;
  std::atomic<bool>& pinged_;
  art_tree<watchman_pending_fs*> tree_;

  struct iterContext {
    const w_string& root;
    PendingCollectionBase& coll;

    int operator()(
        const unsigned char* key,
        uint32_t key_len,
        watchman_pending_fs*& p);

    iterContext(const w_string& root, PendingCollectionBase& coll);
  };
  friend struct iterContext;

  void maybePruneObsoletedChildren(const w_string& path, int flags);
  inline void consolidateItem(watchman_pending_fs* p, int flags);
  bool isObsoletedByContainingDir(const w_string& path);
  inline void linkHead(watchman_pending_fs* p);
  inline void unlinkItem(watchman_pending_fs* p);
};

class PendingCollection
    : public watchman::Synchronized<PendingCollectionBase, std::mutex> {
  std::condition_variable cond_;
  std::atomic<bool> pinged_;

 public:
  PendingCollection();
  LockedPtr lockAndWait(std::chrono::milliseconds timeoutms, bool& pinged);

  // Ping without requiring the lock to be held
  void ping();
};

void w_pending_fs_free(watchman_pending_fs* p);
