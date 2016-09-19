/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <unordered_set>

static void age_out_file(
    struct write_locked_watchman_root* lock,
    std::unordered_set<w_string> &dirs_to_erase,
    struct watchman_file* file) {
  auto parent = file->parent;

  w_string full_name(w_dir_path_cat_str(parent, w_file_get_name(file)), false);
  w_log(W_LOG_DBG, "age_out file=%s\n", full_name.c_str());

  // Revise tick for fresh instance reporting
  lock->root->inner.last_age_out_tick =
      MAX(lock->root->inner.last_age_out_tick, file->otime.ticks);

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

void consider_age_out(struct write_locked_watchman_root *lock)
{
  time_t now;

  if (lock->root->gc_interval == 0) {
    return;
  }

  time(&now);

  if (now <=
      lock->root->inner.last_age_out_timestamp + lock->root->gc_interval) {
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
  struct watchman_file *file, *prior;
  time_t now;
  w_ht_iter_t i;
  w_root_t *root = lock->root;
  uint32_t num_aged_files = 0;
  uint32_t num_aged_cursors = 0;
  uint32_t num_walked = 0;
  std::unordered_set<w_string> dirs_to_erase;

  time(&now);
  root->inner.last_age_out_timestamp = now;
  w_perf_t sample("age_out");

  file = root->inner.latest_file;
  prior = nullptr;
  while (file) {
    ++num_walked;
    if (file->exists || file->otime.timestamp + min_age > now) {
      prior = file;
      file = file->next;
      continue;
    }

    age_out_file(lock, dirs_to_erase, file);
    num_aged_files++;

    // Go back to last good file node; we can't trust that the
    // value of file->next saved before age_out_file is a valid
    // file node as anything past that point may have also been
    // aged out along with it.
    file = prior;
  }

  for (auto& name : dirs_to_erase) {
    auto parent = w_root_resolve_dir(lock, name.dirName(), false);
    if (parent) {
      parent->dirs.erase(name.baseName());
    }
  }

  // Age out cursors too.
  if (w_ht_first(root->inner.cursors, &i)) do {
    if (i.value < root->inner.last_age_out_tick) {
      w_ht_iter_del(root->inner.cursors, &i);
      num_aged_cursors++;
    }
  } while (w_ht_next(root->inner.cursors, &i));

  if (num_aged_files + dirs_to_erase.size() + num_aged_cursors) {
    w_log(
        W_LOG_ERR,
        "aged %" PRIu32 " files, %" PRIu32 " dirs, %" PRIu32 " cursors\n",
        num_aged_files,
        uint32_t(dirs_to_erase.size()),
        num_aged_cursors);
  }
  if (sample.finish()) {
    sample.add_root_meta(lock->root);
    sample.add_meta(
        "age_out",
        json_pack(
            "{s:i, s:i, s:i, s:i}",
            "walked",
            num_walked,
            "files",
            num_aged_files,
            "dirs",
            dirs_to_erase.size(),
            "cursors",
            num_aged_cursors));
    sample.log();
  }
}

/* vim:ts=2:sw=2:et:
 */
