/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {
void InMemoryView::statPath(
    read_locked_watchman_root* lock,
    PendingCollection::LockedPtr& coll,
    const w_string& full_path,
    struct timeval now,
    int flags,
    struct watchman_dir_ent* pre_stat) {
  struct watchman_stat st;
  int res, err;
  char path[WATCHMAN_NAME_MAX];
  bool recursive = flags & W_PENDING_RECURSIVE;
  bool via_notify = flags & W_PENDING_VIA_NOTIFY;
  const w_root_t* root = lock->root;

  if (root->ignore.isIgnoreDir(full_path)) {
    w_log(
        W_LOG_DBG,
        "%.*s matches ignore_dir rules\n",
        int(full_path.size()),
        full_path.data());
    return;
  }

  if (full_path.size() > sizeof(path) - 1) {
    w_log(
        W_LOG_FATAL,
        "path %.*s is too big\n",
        int(full_path.size()),
        full_path.data());
  }

  memcpy(path, full_path.data(), full_path.size());
  path[full_path.size()] = 0;

  auto dir_name = full_path.dirName();
  auto file_name = full_path.baseName();
  auto dir = resolveDir(dir_name, true);

  auto file = dir->getChildFile(file_name);

  auto dir_ent = dir->getChildDir(file_name);

  if (pre_stat && pre_stat->has_stat) {
    memcpy(&st, &pre_stat->stat, sizeof(st));
    res = 0;
    err = 0;
  } else {
    struct stat struct_stat;
    res = w_lstat(path, &struct_stat, root->case_sensitive);
    err = res == 0 ? 0 : errno;
    w_log(W_LOG_DBG, "w_lstat(%s) file=%p dir=%p res=%d %s\n",
        path, file, dir_ent, res, strerror(err));
    if (err == 0) {
      struct_stat_to_watchman_stat(&struct_stat, &st);
    } else {
      // To suppress warning on win32
      memset(&st, 0, sizeof(st));
    }
  }

  if (res && (err == ENOENT || err == ENOTDIR)) {
    /* it's not there, update our state */
    if (dir_ent) {
      markDirDeleted(dir_ent, now, root->inner.ticks, true);
      w_log(
          W_LOG_DBG,
          "w_lstat(%s) -> %s so stopping watch on %.*s\n",
          path,
          strerror(err),
          int(dir_name.size()),
          dir_name.data());
    }
    if (file) {
      if (file->exists) {
        w_log(W_LOG_DBG, "w_lstat(%s) -> %s so marking %.*s deleted\n", path,
              strerror(err), w_file_get_name(file)->len,
              w_file_get_name(file)->buf);
        file->exists = false;
        markFileChanged(file, now, root->inner.ticks);
      }
    } else {
      // It was created and removed before we could ever observe it
      // in the filesystem.  We need to generate a deleted file
      // representation of it now, so that subscription clients can
      // be notified of this event
      file = getOrCreateChildFile(dir, file_name, now, root->inner.ticks);
      w_log(W_LOG_DBG, "w_lstat(%s) -> %s and file node was NULL. "
          "Generating a deleted node.\n", path, strerror(err));
      file->exists = false;
      markFileChanged(file, now, root->inner.ticks);
    }

    if (!root->case_sensitive && !w_string_equal(dir_name, root->root_path) &&
        dir->last_check_existed) {
      /* If we rejected the name because it wasn't canonical,
       * we need to ensure that we look in the parent dir to discover
       * the new item(s) */
      w_log(
          W_LOG_DBG,
          "we're case insensitive, and %s is ENOENT, "
          "speculatively look at parent dir %.*s\n",
          path,
          int(dir_name.size()),
          dir_name.data());
      coll->add(dir_name, now, W_PENDING_CRAWL_ONLY);
    }

  } else if (res) {
    w_log(W_LOG_ERR, "w_lstat(%s) %d %s\n",
        path, err, strerror(err));
  } else {
    if (!file) {
      file = getOrCreateChildFile(dir, file_name, now, root->inner.ticks);
    }

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = root->inner.ticks;
      file->ctime.timestamp = now.tv_sec;
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->stat, &st)) {
      w_log(W_LOG_DBG,
          "file changed exists=%d via_notify=%d stat-changed=%d isdir=%d %s\n",
          (int)file->exists,
          (int)via_notify,
          (int)(file->exists && !via_notify),
          S_ISDIR(st.mode),
          path
      );
      file->exists = true;
      markFileChanged(file, now, root->inner.ticks);
    }

    memcpy(&file->stat, &st, sizeof(file->stat));

#ifndef _WIN32
    // check for symbolic link
    if (S_ISLNK(st.mode)) {
      char link_target_path[WATCHMAN_NAME_MAX];
      ssize_t tlen = 0;

      tlen = readlink(path, link_target_path, sizeof(link_target_path));
      if (tlen < 0 || tlen >= WATCHMAN_NAME_MAX) {
        w_log(W_LOG_ERR,
            "readlink(%s) errno=%d tlen=%d\n", path, errno, (int)tlen);
        file->symlink_target.reset();
      } else {
        bool symlink_changed = false;
        w_string new_symlink_target(link_target_path, tlen, W_STRING_BYTE);
        if (file->symlink_target != new_symlink_target) {
          symlink_changed = true;
        }
        file->symlink_target = new_symlink_target;

        if (symlink_changed && root->config.getBool("watch_symlinks", false)) {
          const_cast<w_root_t*>(root)
              ->inner.pending_symlink_targets.wlock()
              ->add(full_path, now, 0);
        }
      }
    } else {
      file->symlink_target.reset();
    }
#endif

    if (S_ISDIR(st.mode)) {
      if (dir_ent == NULL) {
        recursive = true;
      } else {
        // Ensure that we believe that this node exists
        dir_ent->last_check_existed = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!root->ignore.isIgnoreVCS(dir_name) ||
          // but do if we're looking at the cookie dir (stat_path is never
          // called for the root itself)
          w_string_equal(full_path, root->cookies.cookieDir())) {
        if (!root->inner.watcher->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
          /* we always need to crawl, but may not need to be fully recursive */
          coll->add(
              full_path,
              now,
              W_PENDING_CRAWL_ONLY | (recursive ? W_PENDING_RECURSIVE : 0));
        } else {
          /* we get told about changes on the child, so we only
           * need to crawl if we've never seen the dir before.
           * An exception is that fsevents will only report the root
           * of a dir rename and not a rename event for all of its
           * children. */
          if (recursive) {
            coll->add(
                full_path, now, W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      markDirDeleted(dir_ent, now, root->inner.ticks, true);
    }
    if ((root->inner.watcher->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) &&
        !S_ISDIR(st.mode) && !w_string_equal(dir_name, root->root_path) &&
        dir->last_check_existed) {
      /* Make sure we update the mtime on the parent directory.
       * We're deliberately not propagating any of the flags through; we
       * definitely don't want this to be a recursive evaluation and we
       * won'd want to treat this as VIA_NOTIFY to avoid spuriously
       * marking the node as changed when only its atime was changed.
       * https://github.com/facebook/watchman/issues/305 and
       * https://github.com/facebook/watchman/issues/307 have more
       * context on why this is.
       */
      coll->add(dir_name, now, 0);
    }
  }
}
}

/* vim:ts=2:sw=2:et:
 */
