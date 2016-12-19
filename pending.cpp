/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static const struct flag_map kflags[] = {
  {W_PENDING_CRAWL_ONLY, "CRAWL_ONLY"},
  {W_PENDING_RECURSIVE, "RECURSIVE"},
  {W_PENDING_VIA_NOTIFY, "VIA_NOTIFY"},
  {0, NULL},
};

// Since the tree has no internal knowledge about path structures, when we
// search for "foo/bar" it may return a prefix match for an existing node
// with the key "foo/bard".  We use this function to test whether the string
// exactly matches the input ("foo/bar") or whether it has a slash as the next
// character after the common prefix ("foo/bar/" as a prefix).
static bool is_path_prefix(const char *path, size_t path_len, const char *other,
                           size_t common_prefix) {
  if (common_prefix > path_len) {
    return false;
  }

  w_assert(memcmp(path, other, common_prefix) == 0,
           "is_path_prefix: %.*s vs %.*s should have %d common_prefix chars\n",
           (int)path_len, path, (int)common_prefix, other, (int)common_prefix);

  if (common_prefix == path_len) {
    return true;
  }

  return is_slash(path[common_prefix]);
}

// Helper to un-doubly-link a pending item.
void PendingCollectionBase::unlinkItem(
    std::shared_ptr<watchman_pending_fs>& p) {
  if (pending_ == p) {
    pending_ = p->next;
  }
  auto prev = p->prev.lock();

  if (prev) {
    prev->next = p->next;
  }

  if (p->next) {
    p->next->prev = prev;
  }

  p->next.reset();
  p->prev.reset();
}

// Helper to doubly-link a pending item to the head of a collection.
void PendingCollectionBase::linkHead(std::shared_ptr<watchman_pending_fs>&& p) {
  p->prev.reset();
  p->next = pending_;
  if (p->next) {
    p->next->prev = p;
  }
  pending_ = std::move(p);
}

/* initialize a pending_coll */
PendingCollectionBase::PendingCollectionBase(
    std::condition_variable& cond,
    std::atomic<bool>& pinged)
    : cond_(cond), pinged_(pinged) {}

/* destroy a pending_coll */
PendingCollectionBase::~PendingCollectionBase() {
  drain();
}

/* drain and discard the content of a pending_coll, but do not destroy it */
void PendingCollectionBase::drain() {
  pending_.reset();
  tree_.clear();
}

void PendingCollectionBase::ping() {
  pinged_ = true;
  cond_.notify_all();
}

void PendingCollection::ping() {
  pinged_ = true;
  cond_.notify_all();
}

// Deletion is a bit awkward in this radix tree implementation.
// We can't recursively delete a given prefix as a built-in operation
// and it is non-trivial to add that functionality right now.
// When we lop-off a portion of a tree that we're going to analyze
// recursively, we have to iterate each leaf and explicitly delete
// that leaf.
// Since deletion invalidates the iteration state we have to signal
// to stop iteration after each deletion and then retry the prefix
// deletion.
// We use this kid_context state to pass down the required information
// to the iterator callback so that we adjust the overall state correctly.

PendingCollectionBase::iterContext::iterContext(
    const w_string& root,
    PendingCollectionBase& coll)
    : root(root), coll(coll) {}

// This is the iterator callback we use to prune out obsoleted leaves.
// We need to compare the prefix to make sure that we don't delete
// a sibling node by mistake (see commentary on the is_path_prefix
// function for more on that).
int PendingCollectionBase::iterContext::operator()(
    const w_string& key,
    std::shared_ptr<watchman_pending_fs>& p) {
  if (!p) {
    // It was removed; update the tree to reflect this
    coll.tree_.erase(key);
    // Stop iteration: we deleted something and invalidated the iterators.
    return 1;
  }

  if ((p->flags & W_PENDING_CRAWL_ONLY) == 0 && key.size() > root.size() &&
      is_path_prefix(
          (const char*)key.data(), key.size(), root.data(), root.size()) &&
      !watchman::CookieSync::isPossiblyACookie(p->path)) {
    w_log(
        W_LOG_DBG,
        "delete_kids: removing (%d) %.*s from pending because it is "
        "obsoleted by (%d) %.*s\n",
        int(p->path.size()),
        int(p->path.size()),
        p->path.data(),
        int(root.size()),
        int(root.size()),
        root.data());

    // Unlink the child from the pending index.
    coll.unlinkItem(p);

    // Remove it from the art tree.
    coll.tree_.erase(key);

    // Stop iteration because we just invalidated the iterator state
    // by modifying the tree mid-iteration.
    return 1;
  }

  return 0;
}

// if there are any entries that are obsoleted by a recursive insert,
// walk over them now and mark them as ignored.
void PendingCollectionBase::maybePruneObsoletedChildren(
    w_string path,
    int flags) {
  if ((flags & (W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY)) ==
      W_PENDING_RECURSIVE) {
    iterContext ctx{path, *this};
    uint32_t pruned = 0;
    // Since deletion invalidates the iterator, we need to repeatedly
    // call this to prune out the nodes.  It will return 0 once no
    // matching prefixes are found and deleted.
    while (tree_.iterPrefix((const uint8_t*)path.data(), path.size(), ctx)) {
      // OK; try again
      ++pruned;
    }

    if (pruned) {
      w_log(
          W_LOG_DBG,
          "maybePruneObsoletedChildren: pruned %u nodes under (%d) %.*s\n",
          pruned,
          int(path.size()),
          int(path.size()),
          path.data());
    }
  }
}

void PendingCollectionBase::consolidateItem(watchman_pending_fs* p, int flags) {
  // Increase the strength of the pending item if either of these
  // flags are set.
  // We upgrade crawl-only as well as recursive; it indicates that
  // we've recently just performed the stat and we want to avoid
  // infinitely trying to stat-and-crawl
  p->flags |= flags & (W_PENDING_CRAWL_ONLY|W_PENDING_RECURSIVE);

  maybePruneObsoletedChildren(p->path, p->flags);
}

// Check the tree to see if there is a path that is earlier/higher in the
// filesystem than the input path; if there is, and it is recursive,
// return true to indicate that there is no need to track this new path
// due to the already scheduled higher level path.
bool PendingCollectionBase::isObsoletedByContainingDir(const w_string& path) {
  auto leaf = tree_.longestMatch((const uint8_t*)path.data(), path.size());
  if (!leaf) {
    return false;
  }
  auto p = leaf->value;

  if ((p->flags & W_PENDING_RECURSIVE) && is_path_prefix(
                                              path.data(),
                                              path.size(),
                                              (const char*)leaf->key.data(),
                                              leaf->key.size())) {
    if (watchman::CookieSync::isPossiblyACookie(path)) {
      return false;
    }

    // Yes: the pre-existing entry higher up in the tree obsoletes this
    // one that we would add now.
    w_log(
        W_LOG_DBG,
        "is_obsoleted: SKIP %.*s is obsoleted by %.*s\n",
        int(path.size()),
        path.data(),
        int(p->path.size()),
        p->path.data());
    return true;
  }
  return false;
}

watchman_pending_fs::watchman_pending_fs(
    const w_string& path,
    const struct timeval& now,
    int flags)
    : path(path), now(now), flags(flags) {}

/* add a pending entry.  Will consolidate an existing entry with the
 * same name.  Returns false if an allocation fails.
 * The caller must own the collection lock. */
bool PendingCollectionBase::add(
    const w_string& path,
    struct timeval now,
    int flags) {
  char flags_label[128];

  auto existing = tree_.search(path);
  if (existing) {
    /* Entry already exists: consolidate */
    consolidateItem(existing->get(), flags);
    /* all done */
    return true;
  }

  if (isObsoletedByContainingDir(path)) {
    return true;
  }

  // Try to allocate the new node before we prune any children.
  auto p = std::make_shared<watchman_pending_fs>(path, now, flags);

  maybePruneObsoletedChildren(path, flags);

  w_expand_flags(kflags, flags, flags_label, sizeof(flags_label));
  w_log(
      W_LOG_DBG,
      "add_pending: %.*s %s\n",
      int(path.size()),
      path.data(),
      flags_label);

  tree_.insert(path, p);
  linkHead(std::move(p));

  return true;
}

bool PendingCollectionBase::add(
    struct watchman_dir* dir,
    const char* name,
    struct timeval now,
    int flags) {
  return add(w_dir_path_cat_str(dir, name), now, flags);
}

/* Append the contents of src to target, consolidating in target.
 * src is effectively drained in the process.
 * Caller must own the lock on both src and target. */
void PendingCollectionBase::append(PendingCollectionBase* src) {
  auto p = src->stealItems();
  while (p) {
    auto target_p =
        tree_.search((const uint8_t*)p->path.data(), p->path.size());
    if (target_p) {
      /* Entry already exists: consolidate */
      consolidateItem(target_p->get(), p->flags);
      p = std::move(p->next);
      continue;
    }

    if (isObsoletedByContainingDir(p->path)) {
      p = std::move(p->next);
      continue;
    }
    maybePruneObsoletedChildren(p->path, p->flags);

    auto next = std::move(p->next);
    tree_.insert(p->path, p);
    linkHead(std::move(p));

    p = std::move(next);
  }
}

std::shared_ptr<watchman_pending_fs> PendingCollectionBase::stealItems() {
  tree_.clear();
  return std::move(pending_);
}

/* Returns the number of unique pending items in the collection */
uint32_t PendingCollectionBase::size() const {
  return tree_.size();
}

bool PendingCollectionBase::checkAndResetPinged() {
  if (pending_ || pinged_) {
    pinged_ = false;
    return true;
  }
  return false;
}

PendingCollection::PendingCollection()
    : watchman::Synchronized<PendingCollectionBase, std::mutex>(
          PendingCollectionBase(cond_, pinged_)),
      pinged_(false) {}

PendingCollection::LockedPtr PendingCollection::lockAndWait(
    std::chrono::milliseconds timeoutms,
    bool& pinged) {
  auto lock = wlock();

  if (lock->checkAndResetPinged()) {
    pinged = true;
    return lock;
  }

  if (timeoutms.count() == -1) {
    cond_.wait(lock.getUniqueLock());
  } else {
    cond_.wait_for(lock.getUniqueLock(), timeoutms);
  }

  pinged = lock->checkAndResetPinged();

  return lock;
}

/* vim:ts=2:sw=2:et:
 */
