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

  return path[common_prefix] == WATCHMAN_DIR_SEP
#ifdef _WIN32
         // Windows allows both kinds of slashes
         || path[common_prefix] == '/'
#endif
      ;
}

// Helper to un-doubly-link a pending item.
static inline void unlink_item(struct watchman_pending_collection *coll,
                               struct watchman_pending_fs *p) {
  if (coll->pending == p) {
    coll->pending = p->next;
  }
  if (p->prev) {
    p->prev->next = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  }
}

// Helper to doubly-link a pending item to the head of a collection.
static inline void link_head(struct watchman_pending_collection *coll,
                             struct watchman_pending_fs *p) {
  p->prev = NULL;
  p->next = coll->pending;
  if (coll->pending) {
    coll->pending->prev = p;
  }
  coll->pending = p;
}

/* Free a pending_fs node */
void w_pending_fs_free(struct watchman_pending_fs *p) {
  w_string_delref(p->path);
  free(p);
}

/* initialize a pending_coll */
bool w_pending_coll_init(struct watchman_pending_collection *coll) {
  coll->pending = NULL;
  coll->pinged = false;
  if (pthread_mutex_init(&coll->lock, NULL)) {
    return false;
  }
  if (pthread_cond_init(&coll->cond, NULL)) {
    return false;
  }
  art_tree_init(&coll->tree);
  return true;
}

/* destroy a pending_coll */
void w_pending_coll_destroy(struct watchman_pending_collection *coll) {
  w_pending_coll_drain(coll);
  art_tree_destroy(&coll->tree);
  pthread_mutex_destroy(&coll->lock);
  pthread_cond_destroy(&coll->cond);
}

/* drain and discard the content of a pending_coll, but do not destroy it */
void w_pending_coll_drain(struct watchman_pending_collection *coll) {
  struct watchman_pending_fs *p;

  while ((p = w_pending_coll_pop(coll)) != NULL) {
    w_pending_fs_free(p);
  }

  art_tree_destroy(&coll->tree);
  art_tree_init(&coll->tree);
}

/* compute a deadline on entry, then obtain the collection lock
 * and wait until the deadline expires or until the collection is
 * pinged.  On Return, the caller owns the collection lock. */
bool w_pending_coll_lock_and_wait(struct watchman_pending_collection *coll,
    int timeoutms) {
  struct timespec deadline;
  int errcode;

  if (timeoutms != -1) {
    w_timeoutms_to_abs_timespec(timeoutms, &deadline);
  }
  w_pending_coll_lock(coll);
  if (coll->pending || coll->pinged) {
    coll->pinged = false;
    return true;
  }
  if (timeoutms == -1) {
    errcode = pthread_cond_wait(&coll->cond, &coll->lock);
  } else {
    errcode = pthread_cond_timedwait(&coll->cond, &coll->lock, &deadline);
  }

  return errcode == 0;
}

void w_pending_coll_ping(struct watchman_pending_collection *coll) {
  coll->pinged = true;
  pthread_cond_broadcast(&coll->cond);
}

/* obtain the collection lock */
void w_pending_coll_lock(struct watchman_pending_collection *coll) {
  int err = pthread_mutex_lock(&coll->lock);
  if (err != 0) {
    w_log(W_LOG_FATAL, "lock assertion: %s\n", strerror(err));
  }
}

/* release the collection lock */
void w_pending_coll_unlock(struct watchman_pending_collection *coll) {
  int err = pthread_mutex_unlock(&coll->lock);
  if (err != 0) {
    w_log(W_LOG_FATAL, "unlock assertion: %s\n", strerror(err));
  }
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
struct kid_context {
  w_string_t *root;
  struct watchman_pending_collection *coll;
};

// We need to guarantee that we never collapse a cookie notification
// out of the pending list, because we absolutely must observe it coming
// in via the kernel notification mechanism in order for synchronization
// to be correct.
// Since we don't have a w_root_t available, we can't tell what the
// precise cookie prefix is for the current pending list here, so
// we do a substring match.  Not the most elegant thing in the world.
static inline bool is_possibly_a_cookie(w_string_t *path) {
  return w_string_contains_cstr_len(path, WATCHMAN_COOKIE_PREFIX,
                                    sizeof(WATCHMAN_COOKIE_PREFIX) - 1);
}

// This is the iterator callback we use to prune out obsoleted leaves.
// We need to compare the prefix to make sure that we don't delete
// a sibling node by mistake (see commentary on the is_path_prefix
// function for more on that).
static int delete_kids(void *data, const unsigned char *key, uint32_t key_len,
                       void *value) {
  struct kid_context *ctx = data;
  struct watchman_pending_fs *p = value;
  unused_parameter(value);

  if ((p->flags & W_PENDING_CRAWL_ONLY) == 0 && key_len > ctx->root->len &&
      is_path_prefix((const char *)key, key_len, ctx->root->buf,
                     ctx->root->len) &&
      !is_possibly_a_cookie(p->path)) {

    w_log(W_LOG_DBG,
          "delete_kids: removing (%d) %.*s from pending because it is "
          "obsoleted by (%d) %.*s\n",
          p->path->len, p->path->len, p->path->buf, ctx->root->len,
          ctx->root->len, ctx->root->buf);

    // Unlink the child from the pending index.
    unlink_item(ctx->coll, p);

    // and completely free it.
    w_pending_fs_free(p);

    // Remove it from the art tree also.
    art_delete(&ctx->coll->tree, key, key_len);

    // Stop iteration because we just invalidated the iterator state
    // by modifying the tree mid-iteration.
    return 1;
  }

  return 0;
}

// if there are any entries that are obsoleted by a recursive insert,
// walk over them now and mark them as ignored.
static void
maybe_prune_obsoleted_children(struct watchman_pending_collection *coll,
                               w_string_t *path, int flags) {
  if ((flags & (W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY)) ==
      W_PENDING_RECURSIVE) {
    struct kid_context ctx = {path, coll};
    uint32_t pruned = 0;
    // Since deletion invalidates the iterator, we need to repeatedly
    // call this to prune out the nodes.  It will return 0 once no
    // matching prefixes are found and deleted.
    while (art_iter_prefix(&coll->tree, (const uint8_t *)path->buf, path->len,
                           delete_kids, &ctx)) {
      // OK; try again
      ++pruned;
    }

    if (pruned) {
      w_log(W_LOG_DBG,
            "maybe_prune_obsoleted_children: pruned %u nodes under (%d) %.*s\n",
            pruned, path->len, path->len, path->buf);
    }
  }
}

static inline void consolidate_item(struct watchman_pending_collection *coll,
                                    struct watchman_pending_fs *p, int flags) {
  // Increase the strength of the pending item if either of these
  // flags are set.
  // We upgrade crawl-only as well as recursive; it indicates that
  // we've recently just performed the stat and we want to avoid
  // infinitely trying to stat-and-crawl
  p->flags |= flags & (W_PENDING_CRAWL_ONLY|W_PENDING_RECURSIVE);

  maybe_prune_obsoleted_children(coll, p->path, p->flags);
}

// Check the tree to see if there is a path that is earlier/higher in the
// filesystem than the input path; if there is, and it is recursive,
// return true to indicate that there is no need to track this new path
// due to the already scheduled higher level path.
static bool
is_obsoleted_by_containing_dir(struct watchman_pending_collection *coll,
                               w_string_t *path) {
  struct watchman_pending_fs *p;
  art_leaf *leaf =
      art_longest_match(&coll->tree, (const uint8_t *)path->buf, path->len);
  if (!leaf) {
    return false;
  }
  p = leaf->value;

  if ((p->flags & W_PENDING_RECURSIVE) &&
      is_path_prefix(path->buf, path->len, (const char*)leaf->key, leaf->key_len)) {

    if (is_possibly_a_cookie(path)) {
      return false;
    }

    // Yes: the pre-existing entry higher up in the tree obsoletes this
    // one that we would add now.
    w_log(W_LOG_DBG, "is_obsoleted: SKIP %.*s is obsoleted by %.*s\n",
        path->len, path->buf, p->path->len, p->path->buf);
    return true;
  }
  return false;
}

/* add a pending entry.  Will consolidate an existing entry with the
 * same name.  Returns false if an allocation fails.
 * The caller must own the collection lock. */
bool w_pending_coll_add(struct watchman_pending_collection *coll,
    w_string_t *path, struct timeval now, int flags) {
  struct watchman_pending_fs *p;
  char flags_label[128];

  p = art_search(&coll->tree, (const unsigned char*)path->buf, path->len);
  if (p) {
    /* Entry already exists: consolidate */
    consolidate_item(coll, p, flags);
    /* all done */
    return true;
  }

  if (is_obsoleted_by_containing_dir(coll, path)) {
    return true;
  }

  // Try to allocate the new node before we prune any children.
  p = calloc(1, sizeof(*p));
  if (!p) {
    return false;
  }

  maybe_prune_obsoleted_children(coll, path, flags);

  w_expand_flags(kflags, flags, flags_label, sizeof(flags_label));
  w_log(W_LOG_DBG, "add_pending: %.*s %s\n", path->len, path->buf, flags_label);

  p->flags = flags;
  p->now = now;
  p->path = path;
  w_string_addref(path);

  link_head(coll, p);
  art_insert(&coll->tree, (const uint8_t *)path->buf, path->len, p);

  return true;
}

bool w_pending_coll_add_rel(struct watchman_pending_collection *coll,
    struct watchman_dir *dir, const char *name,
    struct timeval now, int flags)
{
  w_string_t *path_str;
  bool res;

  path_str = w_dir_path_cat_cstr(dir, name);
  if (!path_str) {
    return false;
  }
  res = w_pending_coll_add(coll, path_str, now, flags);
  w_string_delref(path_str);

  return res;
}

/* Append the contents of src to target, consolidating in target.
 * src is effectively drained in the process.
 * Caller must own the lock on both src and target. */
void w_pending_coll_append(struct watchman_pending_collection *target,
    struct watchman_pending_collection *src) {
  struct watchman_pending_fs *p, *target_p;

  while ((p = w_pending_coll_pop(src)) != NULL) {
    target_p = art_search(&target->tree,
        (const uint8_t *)p->path->buf, p->path->len);
    if (target_p) {
      /* Entry already exists: consolidate */
      consolidate_item(target, target_p, p->flags);
      w_pending_fs_free(p);
      continue;
    }

    if (is_obsoleted_by_containing_dir(target, p->path)) {
      w_pending_fs_free(p);
      continue;
    }
    maybe_prune_obsoleted_children(target, p->path, p->flags);

    link_head(target, p);
    art_insert(&target->tree, (const uint8_t *)p->path->buf, p->path->len, p);
  }

  // Empty the src tree and reset it
  art_tree_destroy(&src->tree);
  art_tree_init(&src->tree);
  src->pending = NULL;
}

/* Logically pop an entry from the collection.
 * Does NOT remove the entry from the uniq hash.
 * The intent is that the caller will call this in a tight loop and
 * then _drain() it at the end to clear the uniq hash */
struct watchman_pending_fs *w_pending_coll_pop(
    struct watchman_pending_collection *coll) {
  struct watchman_pending_fs *p = coll->pending;

  if (p) {
    unlink_item(coll, p);
  }

  return p;
}

/* Returns the number of unique pending items in the collection */
uint32_t w_pending_coll_size(struct watchman_pending_collection *coll) {
  return (uint32_t)art_size(&coll->tree);
}

/* vim:ts=2:sw=2:et:
 */
