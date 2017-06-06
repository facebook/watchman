/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include "InMemoryView.h"
#include "watchman.h"
#include "watchman_error_category.h"

static void apply_dir_size_hint(struct watchman_dir *dir,
    uint32_t ndirs, uint32_t nfiles) {
  if (dir->files.empty() && nfiles > 0) {
    dir->files.reserve(nfiles);
  }
  if (dir->dirs.empty() && ndirs > 0) {
    dir->dirs.reserve(ndirs);
  }
}

namespace watchman {
void InMemoryView::crawler(
    const std::shared_ptr<w_root_t>& root,
    SyncView::LockedPtr& view,
    PendingCollection::LockedPtr& coll,
    const w_string& dir_name,
    struct timeval now,
    bool recursive) {
  struct watchman_file *file;
  const watchman_dir_ent* dirent;
  char path[WATCHMAN_NAME_MAX];
  bool stat_all = false;

  if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
    stat_all = watcher_->flags & WATCHER_COALESCED_RENAME;
  } else {
    // If the watcher doesn't give us per-file notifications for
    // watched dirs, then we'll end up explicitly tracking them
    // and will get updates for the files explicitly.
    // We don't need to look at the files again when we crawl
    stat_all = false;
  }

  auto dir = resolveDir(view, dir_name, true);

  memcpy(path, dir_name.data(), dir_name.size());
  path[dir_name.size()] = 0;

  w_log(W_LOG_DBG, "opendir(%s) recursive=%s\n",
      path, recursive ? "true" : "false");

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  std::unique_ptr<watchman_dir_handle> osdir;

  try {
    osdir = watcher_->startWatchDir(root, dir, now, path);
  } catch (const std::system_error& err) {
    handle_open_errno(root, dir, now, "opendir", err.code());
    markDirDeleted(view, dir, now, true);
    return;
  }

  if (dir->files.empty()) {
    // Pre-size our hash(es) if we can, so that we can avoid collisions
    // and re-hashing during initial crawl
    uint32_t num_dirs = 0;
#ifndef _WIN32
    struct stat st;
    int dfd = osdir->getFd();
    if (dfd != -1 && fstat(dfd, &st) == 0) {
      num_dirs = (uint32_t)st.st_nlink;
    }
#endif
    // st.st_nlink is usually number of dirs + 2 (., ..).
    // If it is less than 2 then it doesn't follow that convention.
    // We just pass it through for the dir size hint and the hash
    // table implementation will round that up to the next power of 2
    apply_dir_size_hint(
        dir,
        num_dirs,
        uint32_t(root->config.getInt("hint_num_files_per_dir", 64)));
  }

  /* flag for delete detection */
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists) {
      file->maybe_deleted = true;
    }
  }

  try {
    while ((dirent = osdir->readDir()) != nullptr) {
      // Don't follow parent/self links
      if (dirent->d_name[0] == '.' &&
          (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))) {
        continue;
      }

      // Queue it up for analysis if the file is newly existing
      w_string name(dirent->d_name, W_STRING_BYTE);
      file = dir->getChildFile(name);
      if (file) {
        file->maybe_deleted = false;
      }
      if (!file || !file->exists || stat_all || recursive) {
        auto full_path = w_dir_path_cat_str(dir, name);
        w_log(
            W_LOG_DBG,
            "in crawler calling process_path on %s\n",
            full_path.c_str());
        processPath(
            root,
            view,
            coll,
            full_path,
            now,
            ((recursive || !file || !file->exists) ? W_PENDING_RECURSIVE : 0),
            dirent);
      }
    }
  } catch (const std::system_error& exc) {
    log(ERR,
        "Error while reading dir ",
        path,
        ": ",
        exc.what(),
        ", re-adding to pending list to re-assess\n");
    coll->add(path, now, 0);
  }
  osdir.reset();

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists &&
        (file->maybe_deleted || (file->stat.isDir() && recursive))) {
      coll->add(
          dir,
          file->getName().data(),
          now,
          recursive ? W_PENDING_RECURSIVE : 0);
    }
  }
}
}

/* vim:ts=2:sw=2:et:
 */
