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

  // Flag that we have pending trigger info
  pending_trigger_tick = tick;
  pending_sub_tick = tick;
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

void InMemoryView::markDirDeleted(
    struct watchman_dir* dir,
    const struct timeval& now,
    uint32_t tick,
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
      markFileChanged(file, now, tick);
    }
  }

  if (recursive) {
    for (auto& it : dir->dirs) {
      auto child = it.second.get();

      markDirDeleted(child, now, tick, true);
    }
  }
}

watchman_file* InMemoryView::getOrCreateChildFile(
    watchman_dir* dir,
    const w_string& file_name,
    const struct timeval& now,
    uint32_t tick) {
  w_string_t* name;
  auto& file_ptr = dir->files[file_name];

  if (file_ptr) {
    return file_ptr.get();
  }

  /* We embed our name string in the tail end of the struct that we're
   * allocating here.  This turns out to be more memory efficient due
   * to the way that the allocator bins sizeof(watchman_file); there's
   * a bit of unusable space after the end of the structure that happens
   * to be about the right size to fit a typical filename.
   * Embedding the name in the end allows us to make the most of this
   * memory and free up the separate heap allocation for file_name.
   */
  auto file = (watchman_file*)calloc(
      1, sizeof(watchman_file) + w_string_embedded_size(file_name));
  file_ptr = std::unique_ptr<watchman_file, watchman_dir::Deleter>(
      file, watchman_dir::Deleter());

  name = w_file_get_name(file);
  w_string_embedded_copy(name, file_name);
  w_string_addref(name);

  file->parent = dir;
  file->exists = true;
  file->ctime.ticks = tick;
  file->ctime.timestamp = now.tv_sec;

  auto suffix = file_name.suffix();
  if (suffix) {
    auto& sufhead = suffixes[suffix];
    if (!sufhead) {
      // Create the list head if we don't already have one for this suffix.
      sufhead.reset(new watchman::InMemoryView::file_list_head);
    }

    file->suffix_next = sufhead->head;
    if (file->suffix_next) {
      sufhead->head->suffix_prev = &file->suffix_next;
    }
    sufhead->head = file;
    file->suffix_prev = &sufhead->head;
  }

  watcher->startWatchFile(file);

  return file;
}
}
