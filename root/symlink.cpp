/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <memory>

#ifndef _WIN32
// Given a target of the form "absolute_path/filename", return
// realpath(absolute_path) + filename, where realpath(absolute_path) resolves
// all the symlinks in absolute_path.
static w_string get_normalized_target(const w_string& target) {
  int err;

  w_assert(
      w_string_path_is_absolute(target),
      "get_normalized_target: path %s is not absolute\n",
      target.asNullTerminated().c_str());

  auto dir_name = target.dirName().asNullTerminated();
  auto dir_name_real = autofree(w_realpath(dir_name.c_str()));
  err = errno;

  if (dir_name_real) {
    auto file_name = target.baseName();
    return w_string::pathCat({dir_name_real.get(), file_name});
  }

  errno = err;
  return nullptr;
}

// Requires target to be an absolute path
static void watch_symlink_target(const w_string& target, json_t* root_files) {

  w_assert(
      w_string_path_is_absolute(target),
      "watch_symlink_target: path %s is not absolute\n",
      target.c_str());

  auto normalized_target = get_normalized_target(target);
  if (!normalized_target) {
    w_log(
        W_LOG_ERR,
        "watch_symlink_target: "
        "unable to get normalized version of target `%s`; "
        "realpath errno %d %s\n",
        target.asNullTerminated().c_str(),
        errno,
        strerror(errno));
    return;
  }

  char *relpath = nullptr;
  auto watched_root =
      autofree(w_find_enclosing_root(normalized_target.c_str(), &relpath));
  if (!watched_root) {
    char *errmsg = NULL;
    auto resolved = autofree(w_string_dup_buf(normalized_target));
    if (!find_project_root(root_files, resolved.get(), &relpath)) {
      w_log(
          W_LOG_ERR,
          "watch_symlink_target: No watchable root for %s\n",
          resolved.get());
    } else {
      struct unlocked_watchman_root unlocked;
      bool success = w_root_resolve(resolved.get(), true, &errmsg, &unlocked);

      if (!success) {
        w_log(
            W_LOG_ERR,
            "watch_symlink_target: unable to watch %s: %s\n",
            resolved.get(),
            errmsg);
      } else {
        w_root_delref(&unlocked);
      }
      free(errmsg);
    }
  }
}

/** Given an absolute path, watch all symbolic links associated with the path.
 * Since the target of a symbolic link might contain several components that
 * are themselves symlinks, this function gets called recursively on all the
 * components of path. */
static void watch_symlinks(const w_string& inputPath, json_t* root_files) {
  char link_target_path[WATCHMAN_NAME_MAX];
  ssize_t tlen = 0;

  // We do not currently support symlinks on Windows, so comparing path to "/"
  // is ok
  if (!inputPath || w_string_strlen(inputPath) == 0 ||
      w_string_equal_cstring(inputPath, "/")) {
    return;
  }

  // ensure that buffer is null-terminated
  auto path = inputPath.asNullTerminated();

  w_assert(
      w_string_path_is_absolute(path),
      "watch_symlinks: path %s is not absolute\n",
      path.c_str());

  auto dir_name = path.dirName();
  auto file_name = path.baseName();
  tlen = readlink(path.c_str(), link_target_path, sizeof(link_target_path));

  if (tlen >= (ssize_t)sizeof(link_target_path)) {
    w_log(
        W_LOG_ERR,
        "watch_symlinks: readlink(%s), symlink target is too "
        "long: %d chars >= %d chars\n",
        path.c_str(),
        (int)tlen,
        (int)sizeof(link_target_path));
  } else if (tlen < 0) {
    if (errno == EINVAL) {
      // The final component of path is not a symbolic link, but other
      // components in the path might be symbolic links
      watch_symlinks(dir_name, root_files);
    } else {
      w_log(
          W_LOG_ERR,
          "watch_symlinks: readlink(%s) errno=%d %s tlen=%d\n",
          path.c_str(),
          errno,
          strerror(errno),
          (int)tlen);
    }
  } else {
    w_string target(link_target_path, tlen, W_STRING_BYTE);

    if (w_string_path_is_absolute(target)) {
      watch_symlink_target(target, root_files);
      watch_symlinks(target, root_files);
      watch_symlinks(dir_name, root_files);
    } else {
      auto absolute_target = w_string::pathCat({dir_name, target});

      watch_symlink_target(absolute_target, root_files);
      watch_symlinks(absolute_target, root_files);
      // No need to watch_symlinks(dir_name), since
      // watch_symlinks(absolute_target) will eventually have the same effect
    }
  }
}
#endif  // Symlink-related function definitions excluded for _WIN32

/** Process the list of observed changed symlinks and arrange to establish
 * watches for their new targets */
void process_pending_symlink_targets(struct unlocked_watchman_root *unlocked) {
#ifndef _WIN32
  bool enforcing;

  auto pendingLock = unlocked->root->inner.pending_symlink_targets.wlock();

  if (!pendingLock->size()) {
    return;
  }

  auto root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    w_log(W_LOG_ERR,
          "watch_symlink_target: error computing root_files configuration "
          "value, consult your log file at %s for more details\n", log_name);
    return;
  }

  auto p = pendingLock->stealItems();
  while (p) {
    watch_symlinks(p->path, root_files);
    p = std::move(p->next);
  }

#else
  unused_parameter(unlocked);
#endif
}


/* vim:ts=2:sw=2:et:
 */
