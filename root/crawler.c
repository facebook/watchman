/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void apply_dir_size_hint(struct watchman_dir *dir,
    uint32_t ndirs, uint32_t nfiles) {

  if (nfiles > 0) {
    if (!dir->files) {
      dir->files = w_ht_new(nfiles, &w_ht_string_funcs);
    }
  }
  if (!dir->dirs && ndirs > 0) {
    dir->dirs = w_ht_new(ndirs, &dirname_hash_funcs);
  }
}

void crawler(struct write_locked_watchman_root *lock,
             struct watchman_pending_collection *coll, w_string_t *dir_name,
             struct timeval now, bool recursive) {
  struct watchman_dir *dir;
  struct watchman_file *file;
  struct watchman_dir_handle *osdir;
  struct watchman_dir_ent *dirent;
  w_ht_iter_t i;
  char path[WATCHMAN_NAME_MAX];
  bool stat_all = false;
  w_root_t *root = lock->root;

  if (root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
    stat_all = root->watcher_ops->flags & WATCHER_COALESCED_RENAME;
  } else {
    // If the watcher doesn't give us per-file notifications for
    // watched dirs, then we'll end up explicitly tracking them
    // and will get updates for the files explicitly.
    // We don't need to look at the files again when we crawl
    stat_all = false;
  }

  dir = w_root_resolve_dir(lock, dir_name, true);

  memcpy(path, dir_name->buf, dir_name->len);
  path[dir_name->len] = 0;

  w_log(W_LOG_DBG, "opendir(%s) recursive=%s\n",
      path, recursive ? "true" : "false");

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  osdir = root->watcher_ops->root_start_watch_dir(lock, dir, now, path);
  if (!osdir) {
    return;
  }

  if (!dir->files) {
    // Pre-size our hash(es) if we can, so that we can avoid collisions
    // and re-hashing during initial crawl
    uint32_t num_dirs = 0;
#ifndef _WIN32
    struct stat st;
    int dfd = w_dir_fd(osdir);
    if (dfd != -1 && fstat(dfd, &st) == 0) {
      num_dirs = (uint32_t)st.st_nlink;
    }
#endif
    // st.st_nlink is usually number of dirs + 2 (., ..).
    // If it is less than 2 then it doesn't follow that convention.
    // We just pass it through for the dir size hint and the hash
    // table implementation will round that up to the next power of 2
    apply_dir_size_hint(dir, num_dirs, (uint32_t)cfg_get_int(
                                           root, "hint_num_files_per_dir", 64));
  }

  /* flag for delete detection */
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists) {
      file->maybe_deleted = true;
    }
  } while (w_ht_next(dir->files, &i));

  while ((dirent = w_dir_read(osdir)) != NULL) {
    w_string_t *name;

    // Don't follow parent/self links
    if (dirent->d_name[0] == '.' && (
          !strcmp(dirent->d_name, ".") ||
          !strcmp(dirent->d_name, "..")
        )) {
      continue;
    }

    // Queue it up for analysis if the file is newly existing
    name = w_string_new_typed(dirent->d_name, W_STRING_BYTE);
    if (dir->files) {
      file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(name)));
    } else {
      file = NULL;
    }
    if (file) {
      file->maybe_deleted = false;
    }
    if (!file || !file->exists || stat_all || recursive) {
      w_string_t *full_path = w_dir_path_cat_str(dir, name);
      w_log(W_LOG_DBG, "in crawler calling process_path on %.*s\n",
            full_path->len, full_path->buf);
      w_root_process_path(
          lock, coll, full_path, now,
          ((recursive || !file || !file->exists) ? W_PENDING_RECURSIVE : 0),
          dirent);
      w_string_delref(full_path);
    }
    w_string_delref(name);
  }
  w_dir_close(osdir);

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists && (file->maybe_deleted ||
          (S_ISDIR(file->stat.mode) && recursive))) {
      w_pending_coll_add_rel(coll, dir, w_file_get_name(file)->buf,
          now, recursive ? W_PENDING_RECURSIVE : 0);
    }
  } while (w_ht_next(dir->files, &i));
}

/* vim:ts=2:sw=2:et:
 */
