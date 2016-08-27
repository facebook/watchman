/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void remove_from_suffix_list(struct write_locked_watchman_root *lock,
                                    struct watchman_file *file) {
  w_string_t *suffix = w_string_suffix(w_file_get_name(file));

  if (!suffix) {
    return;
  }

  auto sufhead = (watchman_file*)w_ht_val_ptr(
      w_ht_get(lock->root->suffixes, w_ht_ptr_val(suffix)));
  if (sufhead) {
    if (file->suffix_prev) {
      file->suffix_prev->suffix_next = file->suffix_next;
    }
    if (file->suffix_next) {
      file->suffix_next->suffix_prev = file->suffix_prev;
    }
    if (sufhead == file) {
      sufhead = file->suffix_next;
      w_ht_replace(lock->root->suffixes, w_ht_ptr_val(suffix),
                   w_ht_ptr_val(sufhead));
    }
  }

  w_string_delref(suffix);
}

static void record_aged_out_dir(w_ht_t *aged_dir_names,
                                struct watchman_dir *dir) {
  w_ht_iter_t i;
  w_string_t *full_name = w_dir_copy_full_path(dir);

  w_log(W_LOG_DBG, "age_out: remember dir %.*s\n", full_name->len,
        full_name->buf);

  w_ht_insert(aged_dir_names, w_ht_ptr_val(full_name), w_ht_ptr_val(dir),
              false);

  w_string_delref(full_name);

  if (dir->dirs && w_ht_first(dir->dirs, &i)) do {
    auto child = (watchman_dir*)w_ht_val_ptr(i.value);

    record_aged_out_dir(aged_dir_names, child);
    w_ht_iter_del(dir->dirs, &i);
  } while (w_ht_next(dir->dirs, &i));
}

static void age_out_file(struct write_locked_watchman_root *lock,
                         w_ht_t *aged_dir_names, struct watchman_file *file) {
  struct watchman_dir *dir;
  w_string_t *full_name;

  full_name = w_dir_path_cat_str(file->parent, w_file_get_name(file));
  w_log(W_LOG_DBG, "age_out file=%.*s\n", full_name->len, full_name->buf);

  // Revise tick for fresh instance reporting
  lock->root->last_age_out_tick =
      MAX(lock->root->last_age_out_tick, file->otime.ticks);

  // And remove from the overall file list
  remove_from_file_list(lock, file);
  remove_from_suffix_list(lock, file);

  if (file->parent->files) {
    // Remove the entry from the containing file hash
    w_ht_del(file->parent->files, w_ht_ptr_val(w_file_get_name(file)));
  }

  // resolve the dir of the same name and mark it for later removal
  // from our internal datastructures
  dir = w_root_resolve_dir(lock, full_name, false);
  if (dir) {
    record_aged_out_dir(aged_dir_names, dir);
  } else if (file->parent->dirs) {
    // Remove the entry from the containing dir hash.  This is contingent
    // on not being a dir because in the dir case we want to defer removing
    // the directory entries until later.
    w_ht_del(file->parent->dirs, w_ht_ptr_val(w_file_get_name(file)));
  }

  // And free it.  We don't need to stop watching it, because we already
  // stopped watching it when we marked it as !exists
  free_file_node(lock->root, file);

  w_string_delref(full_name);
}

static void age_out_dir(struct watchman_dir *dir)
{
  assert(!dir->files || w_ht_size(dir->files) == 0);

  // This will implicitly call delete_dir() which will tear down
  // the files and dirs hashes
  w_ht_del(dir->parent->dirs, w_ht_ptr_val(dir->name));
}


void consider_age_out(struct write_locked_watchman_root *lock)
{
  time_t now;

  if (lock->root->gc_interval == 0) {
    return;
  }

  time(&now);

  if (now <= lock->root->last_age_out_timestamp + lock->root->gc_interval) {
    // Don't check too often
    return;
  }

  w_root_perform_age_out(lock, lock->root->gc_age);
}

// Find deleted nodes older than the gc_age setting.
// This is particularly useful in cases where your tree observes a
// large number of creates and deletes for many unique filenames in
// a given dir (eg: temporary/randomized filenames generated as part
// of build tooling or atomic renames)
void w_root_perform_age_out(struct write_locked_watchman_root *lock,
                            int min_age) {
  struct watchman_file *file, *tmp;
  time_t now;
  w_ht_iter_t i;
  w_ht_t *aged_dir_names;
  w_root_t *root = lock->root;

  time(&now);
  root->last_age_out_timestamp = now;
  aged_dir_names = w_ht_new(2, &w_ht_string_funcs);

  file = root->latest_file;
  while (file) {
    if (file->exists || file->otime.timestamp + min_age > now) {
      file = file->next;
      continue;
    }

    // Get the next file before we remove the current one
    tmp = file->next;

    age_out_file(lock, aged_dir_names, file);

    file = tmp;
  }

  // For each dir that matched a pruned file node, delete from
  // our internal structures
  if (w_ht_first(aged_dir_names, &i)) do {
    auto dir = (watchman_dir *)w_ht_val_ptr(i.value);

    age_out_dir(dir);
  } while (w_ht_next(aged_dir_names, &i));
  w_ht_free(aged_dir_names);

  // Age out cursors too.
  if (w_ht_first(root->cursors, &i)) do {
    if (i.value < root->last_age_out_tick) {
      w_ht_iter_del(root->cursors, &i);
    }
  } while (w_ht_next(root->cursors, &i));
}

/* vim:ts=2:sw=2:et:
 */
