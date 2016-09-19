/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void watchman_dir::Deleter::operator()(watchman_file* file) const {
  free_file_node(file);
}

watchman_dir::watchman_dir(w_string name, watchman_dir* parent)
    : name(name), parent(parent) {}

watchman_dir::~watchman_dir() {
  auto full_path = getFullPath();

  w_log(W_LOG_DBG, "delete_dir(%s)\n", full_path.c_str());
}

w_string watchman_dir::getFullPath() const {
  return w_string(w_dir_copy_full_path(this), false);
}

const watchman_dir* w_root_resolve_dir_read(
    struct read_locked_watchman_root* lock,
    w_string_t* dir_name) {
  watchman_dir* dir;
  const char *dir_component;
  const char *dir_end;

  if (w_string_equal(dir_name, lock->root->root_path)) {
    return lock->root->inner.root_dir.get();
  }

  dir_component = dir_name->buf;
  dir_end = dir_component + dir_name->len;

  dir = lock->root->inner.root_dir.get();
  dir_component += lock->root->root_path->len + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
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

    auto child = dir->getChildDir(&component);
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

watchman_dir* w_root_resolve_dir(
    struct write_locked_watchman_root* lock,
    w_string_t* dir_name,
    bool create) {
  watchman_dir *dir, *parent;
  const char *dir_component;
  const char *dir_end;

  if (w_string_equal(dir_name, lock->root->root_path)) {
    return lock->root->inner.root_dir.get();
  }

  dir_component = dir_name->buf;
  dir_end = dir_component + dir_name->len;

  dir = lock->root->inner.root_dir.get();
  dir_component += lock->root->root_path->len + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
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

    auto child = dir->getChildDir(&component);

    if (!child && !create) {
      return NULL;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      w_string child_name(
          dir_component, (uint32_t)(sep - dir_component), dir_name->type);

      auto &new_child = dir->dirs[child_name];
      new_child.reset(new watchman_dir(child_name, dir));

      child = new_child.get();
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

  w_string child_name(
      dir_component, (uint32_t)(dir_end - dir_component), dir_name->type);
  auto &new_child = parent->dirs[child_name];
  new_child.reset(new watchman_dir(child_name, parent));

  dir = new_child.get();

  return dir;
}

watchman_file* watchman_dir::getChildFile(w_string name) const {
  auto it = files.find(name);
  if (it == files.end()) {
    return nullptr;
  }
  return it->second.get();
}

watchman_dir* watchman_dir::getChildDir(w_string name) const {
  auto it = dirs.find(name);
  if (it == dirs.end()) {
    return nullptr;
  }
  return it->second.get();
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(struct write_locked_watchman_root *lock,
                         struct watchman_dir *dir, struct timeval now,
                         bool recursive) {
  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  for (auto& it : dir->files) {
    auto file = it.second.get();

    if (file->exists) {
      w_string_t *full_name = w_dir_path_cat_str(dir, w_file_get_name(file));
      w_log(W_LOG_DBG, "mark_deleted: %.*s\n", full_name->len, full_name->buf);
      w_string_delref(full_name);
      file->exists = false;
      w_root_mark_file_changed(lock, file, now);
    }
  }

  if (recursive) {
    for (auto& it : dir->dirs) {
      auto child = it.second.get();

      w_root_mark_deleted(lock, child, now, true);
    }
  }
}

void stop_watching_dir(struct write_locked_watchman_root *lock,
                       struct watchman_dir *dir) {
  w_ht_iter_t i;
  auto dir_path = dir->getFullPath();

  w_log(W_LOG_DBG, "stop_watching_dir %s\n", dir_path.c_str());

  for (auto& it : dir->dirs) {
    auto child = it.second.get();

    stop_watching_dir(lock, child);
  }

  lock->root->watcher_ops->root_stop_watch_dir(lock, dir);
}

/* vim:ts=2:sw=2:et:
 */
