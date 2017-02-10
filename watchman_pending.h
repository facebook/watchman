/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <chrono>
#include <condition_variable>
#include "thirdparty/libart/src/art.h"
#include "watchman_synchronized.h"

#define W_PENDING_RECURSIVE 1
#define W_PENDING_VIA_NOTIFY 2
#define W_PENDING_CRAWL_ONLY 4

struct watchman_pending_fs {
  // We own the next entry and will destroy that chain when we
  // are destroyed.
  std::shared_ptr<watchman_pending_fs> next;
  std::weak_ptr<watchman_pending_fs> prev;
  w_string path;
  struct timeval now;
  int flags;

  watchman_pending_fs(
      const w_string& path,
      const struct timeval& now,
      int flags);
};

struct PendingCollectionBase {
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

  /* Moves the head of the chain of items to the caller.
   * The tree is cleared and the caller owns the whole chain */
  std::shared_ptr<watchman_pending_fs> stealItems();

  uint32_t size() const;
  void ping();
  bool checkAndResetPinged();

 private:
  std::condition_variable& cond_;
  std::atomic<bool>& pinged_;
  art_tree<std::shared_ptr<watchman_pending_fs>, w_string> tree_;
  std::shared_ptr<watchman_pending_fs> pending_;

  struct iterContext {
    const w_string& root;
    PendingCollectionBase& coll;

    int operator()(
        const w_string& key,
        std::shared_ptr<watchman_pending_fs>& p);

    iterContext(const w_string& root, PendingCollectionBase& coll);
  };
  friend struct iterContext;

  void maybePruneObsoletedChildren(w_string path, int flags);
  inline void consolidateItem(watchman_pending_fs* p, int flags);
  bool isObsoletedByContainingDir(const w_string& path);
  inline void linkHead(std::shared_ptr<watchman_pending_fs>&& p);
  inline void unlinkItem(std::shared_ptr<watchman_pending_fs>& p);
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
