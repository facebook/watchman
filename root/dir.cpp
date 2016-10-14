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
    const w_string& dir_name) {
  return lock->root->inner.view.resolveDir(dir_name);
}

watchman_dir* w_root_resolve_dir(
    struct write_locked_watchman_root* lock,
    const w_string& dir_name,
    bool create) {
  return lock->root->inner.view.resolveDir(dir_name, create);
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
      w_string full_name(w_dir_path_cat_str(dir, w_file_get_name(file)), false);
      w_log(W_LOG_DBG, "mark_deleted: %s\n", full_name.c_str());
      file->exists = false;
      lock->root->inner.view.markFileChanged(
          file, now, lock->root->inner.ticks);
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
  auto dir_path = dir->getFullPath();

  w_log(W_LOG_DBG, "stop_watching_dir %s\n", dir_path.c_str());

  for (auto& it : dir->dirs) {
    auto child = it.second.get();

    stop_watching_dir(lock, child);
  }

  lock->root->inner.watcher->stopWatchDir(lock, dir);
}

/* vim:ts=2:sw=2:et:
 */
