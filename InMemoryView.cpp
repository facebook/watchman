/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include <algorithm>
#include <thread>
#include "make_unique.h"
#include "InMemoryView.h"

namespace watchman {

InMemoryView::view::view(const w_string& root_path)
    : root_dir(watchman::make_unique<watchman_dir>(root_path, nullptr)) {}

InMemoryView::InMemoryView(
    const w_string& root_path,
    CookieSync& cookies,
    Configuration& config)
    : cookies_(cookies),
      config_(config),
      view_(root_path),
      root_path(root_path) {}

void InMemoryView::view::insertAtHeadOfFileList(struct watchman_file* file) {
  file->next = latest_file;
  if (file->next) {
    file->next->prev = &file->next;
  }
  latest_file = file;
  file->prev = &latest_file;
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

  if (view_.latest_file != file) {
    // unlink from list
    remove_from_file_list(file);

    // and move to the head
    view_.insertAtHeadOfFileList(file);
  }

  // Flag that we have pending trigger info
  mostRecentTick_ = tick;
}

const watchman_dir* InMemoryView::resolveDir(const w_string& dir_name) const {
  watchman_dir* dir;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return view_.root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view_.root_dir.get();
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
    return view_.root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view_.root_dir.get();
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
    auto& sufhead = view_.suffixes[suffix];
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

void InMemoryView::ageOutFile(
    std::unordered_set<w_string>& dirs_to_erase,
    watchman_file* file) {
  auto parent = file->parent;

  w_string full_name(w_dir_path_cat_str(parent, w_file_get_name(file)), false);
  w_log(W_LOG_DBG, "age_out file=%s\n", full_name.c_str());

  // Revise tick for fresh instance reporting
  last_age_out_tick = std::max(last_age_out_tick, file->otime.ticks);

  // If we have a corresponding dir, we want to arrange to remove it, but only
  // after we have unlinked all of the associated file nodes.
  dirs_to_erase.insert(full_name);

  // Remove the entry from the containing file hash; this will free it.
  // We don't need to stop watching it, because we already stopped watching it
  // when we marked it as !exists.
  // We remove using the iterator rather than passing the file name in, because
  // the file name will be freed as part of the erasure.
  auto it = parent->files.find(w_file_get_name(file));
  parent->files.erase(it);
}

void InMemoryView::ageOut(w_perf_t& sample, std::chrono::seconds minAge) {
  struct watchman_file *file, *prior;
  time_t now;
  uint32_t num_aged_files = 0;
  uint32_t num_walked = 0;
  std::unordered_set<w_string> dirs_to_erase;

  time(&now);
  last_age_out_timestamp = now;

  file = view_.latest_file;
  prior = nullptr;
  while (file) {
    ++num_walked;
    if (file->exists || file->otime.timestamp + minAge.count() > now) {
      prior = file;
      file = file->next;
      continue;
    }

    ageOutFile(dirs_to_erase, file);
    num_aged_files++;

    // Go back to last good file node; we can't trust that the
    // value of file->next saved before age_out_file is a valid
    // file node as anything past that point may have also been
    // aged out along with it.
    file = prior;
  }

  for (auto& name : dirs_to_erase) {
    auto parent = resolveDir(name.dirName(), false);
    if (parent) {
      parent->dirs.erase(name.baseName());
    }
  }

  if (num_aged_files + dirs_to_erase.size()) {
    w_log(
        W_LOG_ERR,
        "aged %" PRIu32 " files, %" PRIu32 " dirs\n",
        num_aged_files,
        uint32_t(dirs_to_erase.size()));
  }
  sample.add_meta(
      "age_out",
      json_object({{"walked", json_integer(num_walked)},
                   {"files", json_integer(num_aged_files)},
                   {"dirs", json_integer(dirs_to_erase.size())}}));
}

bool InMemoryView::timeGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    int64_t* num_walked) const {
  struct watchman_file* f;
  int64_t n = 0;
  bool result = true;

  // Walk back in time until we hit the boundary
  for (f = view_.latest_file; f; f = f->next) {
    ++n;
    if (ctx->since.is_timestamp && f->otime.timestamp < ctx->since.timestamp) {
      break;
    }
    if (!ctx->since.is_timestamp && f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      result = false;
      goto done;
    }
  }

done:
  *num_walked = n;
  return result;
}

bool InMemoryView::suffixGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    int64_t* num_walked) const {
  struct watchman_file* f;
  int64_t n = 0;
  bool result = true;

  for (const auto& suff : query->suffixes) {
    // Head of suffix index for this suffix
    auto it = view_.suffixes.find(suff);
    if (it == view_.suffixes.end()) {
      continue;
    }

    // Walk and process
    for (f = it->second->head; f; f = f->suffix_next) {
      ++n;
      if (!w_query_file_matches_relative_root(ctx, f)) {
        continue;
      }

      if (!w_query_process_file(query, ctx, f)) {
        result = false;
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

bool InMemoryView::pathGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    int64_t* num_walked) const {
  w_string_t* relative_root;
  struct watchman_file* f;
  int64_t n = 0;
  bool result = true;

  if (query->relative_root) {
    relative_root = query->relative_root;
  } else {
    relative_root = root_path;
  }

  for (const auto& path : query->paths) {
    const watchman_dir* dir;
    w_string_t* file_name;
    w_string dir_name;

    // Compose path with root
    auto full_name = w_string::pathCat({relative_root, path.name});

    // special case of root dir itself
    if (w_string_equal(root_path, full_name)) {
      // dirname on the root is outside the root, which is useless
      dir = resolveDir(full_name);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = full_name.dirName();
    if (!dir_name) {
      continue;
    }

    dir = resolveDir(dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      continue;
    }

    if (!dir->files.empty()) {
      file_name = w_string_basename(path.name);
      f = dir->getChildFile(file_name);
      w_string_delref(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !S_ISDIR(f->stat.mode))) {
        ++n;
        if (!w_query_process_file(query, ctx, f)) {
          result = false;
          goto done;
        }
        continue;
      }
    }

    // Is it a dir?
    if (dir->dirs.empty()) {
      continue;
    }

    dir = dir->getChildDir(full_name.baseName());
  is_dir:
    // We got a dir; process recursively to specified depth
    if (dir) {
      int64_t child_walked = 0;
      result = dirGenerator(query, ctx, dir, path.depth, &child_walked);
      n += child_walked;
      if (!result) {
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

bool InMemoryView::dirGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    const watchman_dir* dir,
    uint32_t depth,
    int64_t* num_walked) const {
  int64_t n = 0;
  bool result = true;

  for (auto& it : dir->files) {
    auto file = it.second.get();
    ++n;

    if (!w_query_process_file(query, ctx, file)) {
      result = false;
      goto done;
    }
  }

  if (depth > 0) {
    for (auto& it : dir->dirs) {
      const auto child = it.second.get();
      int64_t child_walked = 0;

      result = dirGenerator(query, ctx, child, depth - 1, &child_walked);
      n += child_walked;
      if (!result) {
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

bool InMemoryView::allFilesGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    int64_t* num_walked) const {
  struct watchman_file* f;
  int64_t n = 0;
  bool result = true;

  for (f = view_.latest_file; f; f = f->next) {
    ++n;
    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      result = false;
      goto done;
    }
  }

done:
  *num_walked = n;
  return result;
}

uint32_t InMemoryView::getMostRecentTickValue() const {
  return mostRecentTick_.load();
}

uint32_t InMemoryView::getLastAgeOutTickValue() const {
  return last_age_out_tick;
}

time_t InMemoryView::getLastAgeOutTimeStamp() const {
  return last_age_out_timestamp;
}

void InMemoryView::startThreads(w_root_t* root) {
  // Start a thread to call into the watcher API for filesystem notifications
  auto self = std::static_pointer_cast<InMemoryView>(shared_from_this());
  w_log(W_LOG_DBG, "starting threads for %p %s\n", this, root_path.c_str());
  w_root_addref(root);
  std::thread notifyThreadInstance([self, root]() {
    unlocked_watchman_root unlocked{root};
    w_set_thread_name("notify %p %s", self.get(), self->root_path.c_str());
    self->notifyThread(&unlocked);
    w_log(W_LOG_DBG, "out of loop\n");
    w_root_delref(&unlocked);
  });
  notifyThreadInstance.detach();

  // Wait for it to signal that the watcher has been initialized
  bool pinged = false;
  pending_.lockAndWait(std::chrono::milliseconds(-1) /* infinite */, pinged);

  // And now start the IO thread
  w_root_addref(root);
  std::thread ioThreadInstance([self, root]() {
    unlocked_watchman_root unlocked{root};

    w_set_thread_name("io %p %s", self.get(), self->root_path.c_str());
    self->ioThread(&unlocked);
    w_log(W_LOG_DBG, "out of loop\n");
    w_root_delref(&unlocked);
  });
  ioThreadInstance.detach();
}

void InMemoryView::signalThreads() {
  w_log(W_LOG_DBG, "signalThreads! %p %s\n", this, root_path.c_str());
  stopThreads_ = true;
  watcher->signalThreads();
  pending_.ping();
}

bool InMemoryView::doAnyOfTheseFilesExist(
    const std::vector<w_string>& fileNames) const {
  for (auto& name : fileNames) {
    auto fullName = w_string::pathCat({root_path, name});
    const auto dir = resolveDir(fullName.dirName());
    if (!dir) {
      continue;
    }

    auto file = dir->getChildFile(fullName.baseName());
    if (!file) {
      continue;
    }
    if (file->exists) {
      return true;
    }
  }
  return false;
}
}
