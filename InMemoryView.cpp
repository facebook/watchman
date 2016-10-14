/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

#include "InMemoryView.h"

namespace watchman {

InMemoryView::InMemoryView(const w_string& root_path) : root_path(root_path) {}

static void insert_at_head_of_file_list(
    InMemoryView* view,
    struct watchman_file* file) {
  file->next = view->latest_file;
  if (file->next) {
    file->next->prev = &file->next;
  }
  view->latest_file = file;
  file->prev = &view->latest_file;
}

void InMemoryView::markFileChanged(
    watchman_file* file,
    const struct timeval& now,
    uint32_t tick) {
  if (file->exists) {
    watcher->startWatchFile(file);
  }

  file->otime.timestamp = now.tv_sec;
  file->otime.ticks = tick;

  if (latest_file != file) {
    // unlink from list
    remove_from_file_list(file);

    // and move to the head
    insert_at_head_of_file_list(this, file);
  }
}

const watchman_dir* InMemoryView::resolveDir(const w_string& dir_name) const {
  watchman_dir* dir;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    w_string_t component;
    auto sep = (const char*)memchr(
        dir_component, WATCHMAN_DIR_SEP, dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(
        &component,
        dir_component,
        sep ? (uint32_t)(sep - dir_component)
            : (uint32_t)(dir_end - dir_component),
        W_STRING_BYTE);

    auto child = dir->getChildDir(&component);
    if (!child) {
      return nullptr;
    }

    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // Does not exist
      return nullptr;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  return nullptr;
}

watchman_dir* InMemoryView::resolveDir(const w_string& dir_name, bool create) {
  watchman_dir *dir, *parent;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    w_string_t component;
    auto sep = (const char*)memchr(
        dir_component, WATCHMAN_DIR_SEP, dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(
        &component,
        dir_component,
        sep ? (uint32_t)(sep - dir_component)
            : (uint32_t)(dir_end - dir_component),
        W_STRING_BYTE);

    auto child = dir->getChildDir(&component);

    if (!child && !create) {
      return nullptr;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      w_string child_name(dir_component, (uint32_t)(sep - dir_component));

      auto& new_child = dir->dirs[child_name];
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

  w_string child_name(dir_component, (uint32_t)(dir_end - dir_component));
  auto& new_child = parent->dirs[child_name];
  new_child.reset(new watchman_dir(child_name, parent));

  dir = new_child.get();

  return dir;
}
}
