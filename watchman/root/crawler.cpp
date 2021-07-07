/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman.h"
#include "watchman/watchman_system.h"

static void
apply_dir_size_hint(struct watchman_dir* dir, uint32_t ndirs, uint32_t nfiles) {
  if (dir->files.empty() && nfiles > 0) {
    dir->files.reserve(nfiles);
  }
  if (dir->dirs.empty() && ndirs > 0) {
    dir->dirs.reserve(ndirs);
  }
}

namespace watchman {
void InMemoryView::crawler(
    const std::shared_ptr<watchman_root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending) {
  bool recursive = pending.flags & W_PENDING_RECURSIVE;

  bool stat_all;
  if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
    stat_all = watcher_->flags & WATCHER_COALESCED_RENAME;
  } else {
    // If the watcher doesn't give us per-file notifications for watched dirs
    // and is able to watch files individually, then we'll end up explicitly
    // tracking them and will get updates for the files explicitly. We don't
    // need to look at the files again when we crawl. To avoid recursing into
    // all the subdirectories, only stat all the files/directories when this
    // directory was added by the watcher.
    stat_all = pending.flags & W_PENDING_NONRECURSIVE_SCAN;
  }

  auto dir = view.resolveDir(pending.path, true);

  // Detect root directory replacement.
  // The inode number check is handled more generally by the sister code
  // in stat.cpp.  We need to special case it for the root because we never
  // generate a watchman_file node for the root and thus never call
  // InMemoryView::statPath (we'll fault if we do!).
  // Ideally the kernel would have given us a signal when we've been replaced
  // but some filesystems (eg: BTRFS) do not emit appropriate inotify events
  // for things like subvolume deletes.  We've seen situations where the
  // root has been replaced and we got no notifications at all and this has
  // left the cookie sync mechanism broken forever.
  if (pending.path == root->root_path) {
    try {
      auto st = getFileInformation(pending.path.c_str(), root->case_sensitive);
      if (st.ino != view.getRootInode()) {
        // If it still exists and the inode doesn't match, then we need
        // to force recrawl to make sure we're in sync.
        // We're lazily initializing the rootInode to 0 here, so we don't
        // need to do this the first time through (we're already crawling
        // everything in that case).
        if (view.getRootInode() != 0) {
          root->scheduleRecrawl(
              "root was replaced and we didn't get notified by the kernel");
          return;
        }
        recursive = true;
        view.setRootInode(st.ino);
      }
    } catch (const std::system_error& err) {
      handle_open_errno(
          *root, dir, pending.now, "getFileInformation", err.code());
      view.markDirDeleted(*watcher_, dir, getClock(pending.now), true);
      return;
    }
  }

  char path[WATCHMAN_NAME_MAX];
  memcpy(path, pending.path.data(), pending.path.size());
  path[pending.path.size()] = 0;

  logf(
      DBG, "opendir({}) recursive={} stat_all={}\n", path, recursive, stat_all);

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  std::unique_ptr<watchman_dir_handle> osdir;

  try {
    osdir = watcher_->startWatchDir(root, dir, path);
  } catch (const std::system_error& err) {
    logf(DBG, "startWatchDir({}) threw {}\n", path, err.what());
    handle_open_errno(*root, dir, pending.now, "opendir", err.code());
    view.markDirDeleted(*watcher_, dir, getClock(pending.now), true);
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
    while (const watchman_dir_ent* dirent = osdir->readDir()) {
      // Don't follow parent/self links
      if (dirent->d_name[0] == '.' &&
          (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))) {
        continue;
      }

      // Queue it up for analysis if the file is newly existing
      w_string name(dirent->d_name, W_STRING_BYTE);
      struct watchman_file* file = dir->getChildFile(name);
      if (file) {
        file->maybe_deleted = false;
      }
      if (!file || !file->exists || stat_all || recursive) {
        auto full_path = dir->getFullPathToChild(name);

        int newFlags = 0;
        if (recursive || !file || !file->exists) {
          newFlags |= W_PENDING_RECURSIVE;
        }
        if (pending.flags & W_PENDING_IS_DESYNCED) {
          newFlags |= W_PENDING_IS_DESYNCED;
        }

        logf(
            DBG,
            "in crawler calling processPath on {} oldflags={} newflags={}\n",
            full_path,
            pending.flags,
            newFlags);

        processPath(
            root,
            view,
            coll,
            PendingChange{std::move(full_path), pending.now, newFlags},
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
    coll.add(path, pending.now, 0);
  }
  osdir.reset();

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists &&
        (file->maybe_deleted || (file->stat.isDir() && recursive))) {
      coll.add(
          dir,
          file->getName().data(),
          pending.now,
          recursive ? W_PENDING_RECURSIVE : 0);
    }
  }
}
} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
