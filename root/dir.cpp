/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void delete_dir(struct watchman_dir *dir) {
  w_string_t *full_path = w_dir_copy_full_path(dir);

  w_log(W_LOG_DBG, "delete_dir(%.*s)\n", full_path->len, full_path->buf);
  w_string_delref(full_path);

  if (dir->files) {
    w_ht_free(dir->files);
    dir->files = NULL;
  }

  if (dir->dirs) {
    w_ht_free(dir->dirs);
    dir->dirs = NULL;
  }

  w_string_delref(dir->name);
  free(dir);
}

static void delete_dir_helper(w_ht_val_t val) {
  delete_dir((struct watchman_dir*)w_ht_val_ptr(val));
}

const struct watchman_hash_funcs dirname_hash_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  delete_dir_helper
};

struct watchman_dir *
w_root_resolve_dir_read(struct read_locked_watchman_root *lock,
                        w_string_t *dir_name) {
  struct watchman_dir *dir;
  const char *dir_component;
  const char *dir_end;

  if (w_string_equal(dir_name, lock->root->root_path)) {
    return lock->root->root_dir;
  }

  dir_component = dir_name->buf;
  dir_end = dir_component + dir_name->len;

  dir = lock->root->root_dir;
  dir_component += lock->root->root_path->len + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    struct watchman_dir *child;
    w_string_t component;
    auto sep = (const char*)memchr(
        dir_component, WATCHMAN_DIR_SEP, dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(&component, dir_component,
                                 sep ? (uint32_t)(sep - dir_component)
                                     : (uint32_t)(dir_end - dir_component),
                                 dir_name->type);

    child = dir->dirs ? (watchman_dir*)w_ht_val_ptr(
                            w_ht_get(dir->dirs, w_ht_ptr_val(&component)))
                      : NULL;
    if (!child) {
      return NULL;
    }

    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // Does not exist
      return NULL;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  return NULL;
}

struct watchman_dir *w_root_resolve_dir(struct write_locked_watchman_root *lock,
                                        w_string_t *dir_name, bool create) {
  struct watchman_dir *dir, *parent;
  const char *dir_component;
  const char *dir_end;

  if (w_string_equal(dir_name, lock->root->root_path)) {
    return lock->root->root_dir;
  }

  dir_component = dir_name->buf;
  dir_end = dir_component + dir_name->len;

  dir = lock->root->root_dir;
  dir_component += lock->root->root_path->len + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    struct watchman_dir *child;
    w_string_t component;
    auto sep = (const char*)memchr(
        dir_component, WATCHMAN_DIR_SEP, dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(&component, dir_component,
                                 sep ? (uint32_t)(sep - dir_component)
                                     : (uint32_t)(dir_end - dir_component),
                                 dir_name->type);

    child = dir->dirs ? (watchman_dir*)w_ht_val_ptr(
                            w_ht_get(dir->dirs, w_ht_ptr_val(&component)))
                      : NULL;
    if (!child && !create) {
      return NULL;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      child = (watchman_dir*)calloc(1, sizeof(*child));
      child->name = w_string_new_len_typed(
          dir_component, (uint32_t)(sep - dir_component), dir_name->type);
      child->last_check_existed = true;
      child->parent = dir;

      if (!dir->dirs) {
        dir->dirs = w_ht_new(2, &dirname_hash_funcs);
      }

      w_ht_set(dir->dirs, w_ht_ptr_val(child->name), w_ht_ptr_val(child));
    }

    parent = dir;
    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // We need to create the dir
      break;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  dir = (watchman_dir*)calloc(1, sizeof(*dir));
  dir->name = w_string_new_len_typed(
      dir_component, (uint32_t)(dir_end - dir_component), dir_name->type);
  dir->last_check_existed = true;
  dir->parent = parent;

  if (!parent->dirs) {
    parent->dirs = w_ht_new(2, &dirname_hash_funcs);
  }

  w_ht_set(parent->dirs, w_ht_ptr_val(dir->name), w_ht_ptr_val(dir));

  return dir;
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(struct write_locked_watchman_root *lock,
                         struct watchman_dir *dir, struct timeval now,
                         bool recursive) {
  w_ht_iter_t i;

  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  if (w_ht_first(dir->files, &i)) do {
    auto file = (watchman_file *)w_ht_val_ptr(i.value);

    if (file->exists) {
      w_string_t *full_name = w_dir_path_cat_str(dir, w_file_get_name(file));
      w_log(W_LOG_DBG, "mark_deleted: %.*s\n", full_name->len, full_name->buf);
      w_string_delref(full_name);
      file->exists = false;
      w_root_mark_file_changed(lock, file, now);
    }

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    auto child = (watchman_dir *)w_ht_val_ptr(i.value);

    w_root_mark_deleted(lock, child, now, true);
  } while (w_ht_next(dir->dirs, &i));
}

void stop_watching_dir(struct write_locked_watchman_root *lock,
                       struct watchman_dir *dir) {
  w_ht_iter_t i;
  w_string_t *dir_path = w_dir_copy_full_path(dir);

  w_log(W_LOG_DBG, "stop_watching_dir %.*s\n", dir_path->len, dir_path->buf);
  w_string_delref(dir_path);

  if (w_ht_first(dir->dirs, &i)) do {
    auto child = (watchman_dir *)w_ht_val_ptr(i.value);

    stop_watching_dir(lock, child);
  } while (w_ht_next(dir->dirs, &i));

  lock->root->watcher_ops->root_stop_watch_dir(lock, dir);
}

/* vim:ts=2:sw=2:et:
 */
