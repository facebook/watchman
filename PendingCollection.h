/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include "watchman_string.h"
#include <folly/Synchronized.h>
#include <chrono>
#include <condition_variable>
#include "thirdparty/libart/src/art.h"

namespace watchman {

/**
 * Set when this change requires a recursive scan of its children.
 */
#define W_PENDING_RECURSIVE 1
/**
 * This change event came from a watcher.
 */
#define W_PENDING_VIA_NOTIFY 2
/**
 *
 */
#define W_PENDING_CRAWL_ONLY 4
/**
 * Set this flag when the watcher is desynced and may have missed filesystem
 * events. The W_PENDING_RECURSIVE flag should also be set alongside it to
 * force an recrawl of the passed in directory. Cookies will not be considered
 * when this flag is set.
 */
#define W_PENDING_IS_DESYNCED 8

/**
 * Represents a change notification from the Watcher.
 */
struct PendingChange {
  w_string path;
  struct timeval now;
  int flags;
};

} // namespace watchman

struct watchman_pending_fs : watchman::PendingChange {
  // We own the next entry and will destroy that chain when we
  // are destroyed.
  std::shared_ptr<watchman_pending_fs> next;

  watchman_pending_fs(w_string path, const struct timeval& now, int flags)
      : PendingChange{std::move(path), now, flags} {}

 private:
  // Only used for unlinking during pruning.
  std::weak_ptr<watchman_pending_fs> prev;
  friend class PendingCollectionBase;
};

class PendingCollectionBase {
 public:
  PendingCollectionBase(
      std::condition_variable& cond,
      std::atomic<bool>& pinged);
  PendingCollectionBase(PendingCollectionBase&&) = delete;
  PendingCollectionBase& operator=(PendingCollectionBase&&) = delete;
  ~PendingCollectionBase() = default;

  /**
   * Erase all elements from the collection.
   */
  void clear();

  /**
   * Add a pending entry.  Will consolidate an existing entry with the same
   * name. The caller must own the collection lock.
   */
  void add(const w_string& path, struct timeval now, int flags);
  void add(
      struct watchman_dir* dir,
      const char* name,
      struct timeval now,
      int flags);

  /**
   * Merge the full contents of `chain` into this collection. They are usually
   * from a stealItems() call.
   *
   * `chain` is consumed -- the links are broken.
   */
  void append(std::shared_ptr<watchman_pending_fs> chain);

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

  void maybePruneObsoletedChildren(w_string path, int flags);
  inline void consolidateItem(watchman_pending_fs* p, int flags);
  bool isObsoletedByContainingDir(const w_string& path);
  inline void linkHead(std::shared_ptr<watchman_pending_fs>&& p);
  inline void unlinkItem(std::shared_ptr<watchman_pending_fs>& p);
};

class PendingCollection
    : public folly::Synchronized<PendingCollectionBase, std::mutex> {
 public:
  PendingCollection();
  LockedPtr lockAndWait(std::chrono::milliseconds timeoutms, bool& pinged);

  // Ping without requiring the lock to be held
  void ping();

 private:
  std::condition_variable cond_;
  std::atomic<bool> pinged_;
};

namespace watchman {

// Since the tree has no internal knowledge about path structures, when we
// search for "foo/bar" it may return a prefix match for an existing node
// with the key "foo/bard".  We use this function to test whether the string
// exactly matches the input ("foo/bar") or whether it has a slash as the next
// character after the common prefix ("foo/bar/" as a prefix).
bool is_path_prefix(
    const char* path,
    size_t path_len,
    const char* other,
    size_t common_prefix);

inline bool is_path_prefix(const w_string& key, const w_string& root) {
  return is_path_prefix(key.data(), key.size(), root.data(), root.size());
}

} // namespace watchman
