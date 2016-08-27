/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void watch_file(struct write_locked_watchman_root *lock,
                       struct watchman_file *file) {
  lock->root->watcher_ops->root_start_watch_file(lock, file);
}

static void stop_watching_file(struct write_locked_watchman_root *lock,
                               struct watchman_file *file) {
  lock->root->watcher_ops->root_stop_watch_file(lock, file);
}

void remove_from_file_list(struct write_locked_watchman_root *lock,
                           struct watchman_file *file) {
  if (lock->root->latest_file == file) {
    lock->root->latest_file = file->next;
  }
  if (file->next) {
    file->next->prev = file->prev;
  }
  if (file->prev) {
    file->prev->next = file->next;
  }
}

void w_root_mark_file_changed(struct write_locked_watchman_root *lock,
                              struct watchman_file *file, struct timeval now) {
  if (file->exists) {
    watch_file(lock, file);
  } else {
    stop_watching_file(lock, file);
  }

  file->otime.timestamp = now.tv_sec;
  file->otime.ticks = lock->root->ticks;

  if (lock->root->latest_file != file) {
    // unlink from list
    remove_from_file_list(lock, file);

    // and move to the head
    file->next = lock->root->latest_file;
    if (file->next) {
      file->next->prev = file;
    }
    file->prev = NULL;
    lock->root->latest_file = file;
  }

  // Flag that we have pending trigger info
  lock->root->pending_trigger_tick = lock->root->ticks;
  lock->root->pending_sub_tick = lock->root->ticks;
}

struct watchman_file *
w_root_resolve_file(struct write_locked_watchman_root *lock,
                    struct watchman_dir *dir, w_string_t *file_name,
                    struct timeval now) {
  struct watchman_file *file, *sufhead;
  w_string_t *suffix;
  w_string_t *name;

  if (dir->files) {
    file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
    if (file) {
      return file;
    }
  } else {
    dir->files = w_ht_new(2, &w_ht_string_funcs);
  }

  /* We embed our name string in the tail end of the struct that we're
   * allocating here.  This turns out to be more memory efficient due
   * to the way that the allocator bins sizeof(watchman_file); there's
   * a bit of unusable space after the end of the structure that happens
   * to be about the right size to fit a typical filename.
   * Embedding the name in the end allows us to make the most of this
   * memory and free up the separate heap allocation for file_name.
   */
  file = calloc(1, sizeof(*file) + w_string_embedded_size(file_name));
  name = w_file_get_name(file);
  w_string_embedded_copy(name, file_name);
  w_string_addref(name);

  file->parent = dir;
  file->exists = true;
  file->ctime.ticks = lock->root->ticks;
  file->ctime.timestamp = now.tv_sec;

  suffix = w_string_suffix(file_name);
  if (suffix) {
    sufhead =
        w_ht_val_ptr(w_ht_get(lock->root->suffixes, w_ht_ptr_val(suffix)));
    file->suffix_next = sufhead;
    if (sufhead) {
      sufhead->suffix_prev = file;
    }
    w_ht_replace(lock->root->suffixes, w_ht_ptr_val(suffix),
                 w_ht_ptr_val(file));
    w_string_delref(suffix);
  }

  w_ht_set(dir->files, w_ht_ptr_val(name), w_ht_ptr_val(file));
  watch_file(lock, file);

  return file;
}

/* vim:ts=2:sw=2:et:
 */
