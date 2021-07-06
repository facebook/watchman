/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman.h"

namespace watchman {

/**
 * The purpose of this function is to help us decide whether we should
 * update the parent directory when a non-directory directory entry is changed.
 * If so, we schedule re-examining the parent. Not all systems report the
 * containing directory as changed in that situation, so we decide this based on
 * the capabilities of the watcher. If the directory is added to the
 * PendingCollection, this function returns true. Otherwise, this function
 * returns false.
 */
bool InMemoryView::propagateToParentDirIfAppropriate(
    const watchman_root& root,
    PendingChanges& coll,
    std::chrono::system_clock::time_point now,
    const FileInformation& entryStat,
    const w_string& dirName,
    const watchman_dir* parentDir,
    bool isUnlink) {
  if ((watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) &&
      dirName != root.root_path && !entryStat.isDir() &&
      parentDir->last_check_existed) {
    /* We're deliberately not propagating any of the flags through
     * from statPath() (which calls us); we
     * definitely don't want this to be a recursive evaluation.
     * Previously, we took pains to avoid turning on VIA_NOTIFY
     * here to avoid spuriously marking the node as changed when
     * only its atime was changed to avoid tickling some behavior
     * in the Pants build system:
     * https://github.com/facebook/watchman/issues/305 and
     * https://github.com/facebook/watchman/issues/307, but
     * unfortunately we do need to set it here because eg:
     * Linux doesn't send an inotify event for the parent
     * directory for an unlink, and if we rely on stat()
     * alone, the filesystem mtime granularity may be too
     * low for us to detect that the parent has changed.
     * As a compromize, if we're told that the change was due
     * to an unlink, then we force delivery of a change event,
     * otherwise we'll only do so if the directory has
     * observably changed via stat().
     */
    coll.add(dirName, now, isUnlink ? W_PENDING_VIA_NOTIFY : 0);
    return true;
  }
  return false;
}

void InMemoryView::statPath(
    watchman_root& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const watchman_dir_ent* pre_stat) {
  bool recursive = pending.flags & W_PENDING_RECURSIVE;
  const bool via_notify = pending.flags & W_PENDING_VIA_NOTIFY;
  const int desynced_flag = pending.flags & W_PENDING_IS_DESYNCED;

  if (root.ignore.isIgnoreDir(pending.path)) {
    logf(DBG, "{} matches ignore_dir rules\n", pending.path);
    return;
  }

  char path[WATCHMAN_NAME_MAX];
  if (pending.path.size() > sizeof(path) - 1) {
    logf(FATAL, "path {} is too big\n", pending.path);
  }

  memcpy(path, pending.path.data(), pending.path.size());
  path[pending.path.size()] = 0;

  auto dir_name = pending.path.dirName();
  auto file_name = pending.path.baseName();
  auto parentDir = view.resolveDir(dir_name, true);

  auto file = parentDir->getChildFile(file_name);

  auto dir_ent = parentDir->getChildDir(file_name);

  watchman::FileInformation st;
  std::error_code errcode;
  if (pre_stat && pre_stat->has_stat) {
    st = pre_stat->stat;
  } else {
    try {
      st = getFileInformation(path, root.case_sensitive);
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          "\n");
    } catch (const std::system_error& exc) {
      errcode = exc.code();
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          " failed: ",
          exc.what(),
          "\n");
    }
  }

  if (processedPaths_) {
    processedPaths_->write(PendingChangeLogEntry{pending, errcode, st});
  }

  if (errcode == watchman::error_code::no_such_file_or_directory ||
      errcode == watchman::error_code::not_a_directory) {
    /* it's not there, update our state */
    if (dir_ent) {
      view.markDirDeleted(*watcher_, dir_ent, getClock(pending.now), true);
      watchman::log(
          watchman::DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " so stopping watch\n");
    }
    if (file) {
      if (file->exists) {
        watchman::log(
            watchman::DBG,
            "getFileInformation(",
            path,
            ") -> ",
            errcode.message(),
            " so marking ",
            file->getName(),
            " deleted\n");
        file->exists = false;
        view.markFileChanged(*watcher_, file, getClock(pending.now));
      }
    } else {
      // It was created and removed before we could ever observe it
      // in the filesystem.  We need to generate a deleted file
      // representation of it now, so that subscription clients can
      // be notified of this event
      file = view.getOrCreateChildFile(
          *watcher_, parentDir, file_name, getClock(pending.now));
      log(DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " and file node was NULL. "
          "Generating a deleted node.\n");
      file->exists = false;
      view.markFileChanged(*watcher_, file, getClock(pending.now));
    }

    if (!propagateToParentDirIfAppropriate(
            root,
            coll,
            pending.now,
            file->stat,
            dir_name,
            parentDir,
            /* isUnlink= */ true) &&
        root.case_sensitive == CaseSensitivity::CaseInSensitive &&
        !w_string_equal(dir_name, root.root_path) &&
        parentDir->last_check_existed) {
      /* If we rejected the name because it wasn't canonical,
       * we need to ensure that we look in the parent dir to discover
       * the new item(s) */
      logf(
          DBG,
          "we're case insensitive, and {} is ENOENT, "
          "speculatively look at parent dir {}\n",
          path,
          dir_name);
      coll.add(dir_name, pending.now, W_PENDING_CRAWL_ONLY);
    }

  } else if (errcode.value()) {
    log(ERR,
        "getFileInformation(",
        path,
        ") failed and not handled! -> ",
        errcode.message(),
        " value=",
        errcode.value(),
        " category=",
        errcode.category().name(),
        "\n");
  } else {
    if (!file) {
      file = view.getOrCreateChildFile(
          *watcher_, parentDir, file_name, getClock(pending.now));
    }

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = mostRecentTick_;
      file->ctime.timestamp = std::chrono::system_clock::to_time_t(pending.now);
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->stat, &st)) {
      logf(
          DBG,
          "file changed exists={} via_notify={} stat-changed={} isdir={} {}\n",
          file->exists,
          via_notify,
          file->exists && !via_notify,
          st.isDir(),
          path);
      file->exists = true;
      view.markFileChanged(*watcher_, file, getClock(pending.now));

      // If the inode number changed then we definitely need to recursively
      // examine any children because we cannot assume that the kernel will
      // have given us the correct hints about this change.  BTRFS is one
      // example of a filesystem where this has been observed to happen.
      if (file->stat.ino != st.ino) {
        recursive = true;
      }
    }

    memcpy(&file->stat, &st, sizeof(file->stat));

    // check for symbolic link
    if (st.isSymlink() && root.config.getBool("watch_symlinks", false)) {
      root.inner.pending_symlink_targets.lock()->add(
          pending.path, pending.now, 0);
    }

    if (st.isDir()) {
      if (dir_ent == NULL) {
        recursive = true;
      } else {
        // Ensure that we believe that this node exists
        dir_ent->last_check_existed = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!root.ignore.isIgnoreVCS(dir_name) ||
          // but do if we're looking at the cookie dir (stat_path is never
          // called for the root itself)
          root.cookies.isCookieDir(pending.path)) {
        if (recursive) {
          /* we always need to crawl if we're recursive, this can happen when a
           * directory is created */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY);
        } else if (pending.flags & W_PENDING_NONRECURSIVE_SCAN) {
          /* on file changes, we receive a notification on the directory and
           * thus we just need to crawl this one directory to consider all
           * the pending files. */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_NONRECURSIVE_SCAN |
                  W_PENDING_CRAWL_ONLY);
        } else {
          if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
            /* we get told about changes on the child, so we don't need to do
             * anything */
          } else {
            /* in all the other cases, crawl */
            coll.add(
                pending.path,
                pending.now,
                desynced_flag | W_PENDING_CRAWL_ONLY);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      view.markDirDeleted(*watcher_, dir_ent, getClock(pending.now), true);
    }
    propagateToParentDirIfAppropriate(
        root,
        coll,
        pending.now,
        st,
        dir_name,
        parentDir,
        /* isUnlink= */ false);
  }
}
} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
