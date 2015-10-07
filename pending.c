/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Free a pending_fs node */
void w_pending_fs_free(struct watchman_pending_fs *p) {
  w_string_delref(p->path);
  free(p);
}

/* initialize a pending_coll */
bool w_pending_coll_init(struct watchman_pending_collection *coll) {
  coll->pending = NULL;
  coll->pinged = false;
  coll->pending_uniq = w_ht_new(WATCHMAN_BATCH_LIMIT, &w_ht_string_funcs);
  if (!coll->pending_uniq) {
    return false;
  }
  if (pthread_mutex_init(&coll->lock, NULL)) {
    return false;
  }
  if (pthread_cond_init(&coll->cond, NULL)) {
    return false;
  }
  return true;
}

/* destroy a pending_coll */
void w_pending_coll_destroy(struct watchman_pending_collection *coll) {
  w_pending_coll_drain(coll);
  w_ht_free(coll->pending_uniq);
  pthread_mutex_destroy(&coll->lock);
  pthread_cond_destroy(&coll->cond);
}

/* drain and discard the content of a pending_coll, but do not destroy it */
void w_pending_coll_drain(struct watchman_pending_collection *coll) {
  struct watchman_pending_fs *p;

  while ((p = w_pending_coll_pop(coll)) != NULL) {
    w_pending_fs_free(p);
  }

  w_ht_free_entries(coll->pending_uniq);
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

static inline void consolidate_item(struct watchman_pending_fs *p,
    int flags) {
  // Increase the strength of the pending item if either of these
  // flags are set.
  // We upgrade crawl-only as well as recursive; it indicates that
  // we've recently just performed the stat and we want to avoid
  // infinitely trying to stat-and-crawl
  p->flags |= flags & (W_PENDING_CRAWL_ONLY|W_PENDING_RECURSIVE);
}

/* add a pending entry.  Will consolidate an existing entry with the
 * same name.  Returns false if an allocation fails.
 * The caller must own the collection lock. */
bool w_pending_coll_add(struct watchman_pending_collection *coll,
    w_string_t *path, struct timeval now, int flags) {
  struct watchman_pending_fs *p;

  p = w_ht_val_ptr(w_ht_get(coll->pending_uniq, w_ht_ptr_val(path)));
  if (p) {
    /* Entry already exists: consolidate */
    consolidate_item(p, flags);
    /* all done */
    return true;
  }

  p = calloc(1, sizeof(*p));
  if (!p) {
    return false;
  }

  w_log(W_LOG_DBG, "add_pending: %.*s\n", path->len, path->buf);

  p->flags = flags;
  p->now = now;
  p->path = path;
  w_string_addref(path);

  p->next = coll->pending;
  coll->pending = p;
  w_ht_set(coll->pending_uniq, w_ht_ptr_val(path), w_ht_ptr_val(p));

  return true;
}

bool w_pending_coll_add_rel(struct watchman_pending_collection *coll,
    struct watchman_dir *dir, const char *name,
    struct timeval now, int flags)
{
  w_string_t *path_str;
  bool res;

  path_str = w_string_path_cat_cstr(dir->path, name);
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
    target_p = w_ht_val_ptr(w_ht_get(target->pending_uniq,
                            w_ht_ptr_val(p->path)));
    if (target_p) {
      /* Entry already exists: consolidate */
      consolidate_item(target_p, p->flags);
      w_pending_fs_free(p);
      continue;
    }

    p->next = target->pending;
    target->pending = p;
    w_ht_set(target->pending_uniq, w_ht_ptr_val(p->path), w_ht_ptr_val(p));
  }

  w_ht_free_entries(src->pending_uniq);
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
    coll->pending = p->next;
    p->next = NULL;
  }

  return p;
}

/* Returns the number of unique pending items in the collection */
uint32_t w_pending_coll_size(struct watchman_pending_collection *coll) {
  return w_ht_size(coll->pending_uniq);
}

/* vim:ts=2:sw=2:et:
 */
