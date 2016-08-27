/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifndef _WIN32
// Given a target of the form "absolute_path/filename", return
// realpath(absolute_path) + filename, where realpath(absolute_path) resolves
// all the symlinks in absolute_path.
static w_string_t *get_normalized_target(w_string_t *target) {
  w_string_t *dir_name, *file_name, *normalized_target = NULL;
  char *dir_name_buf, *dir_name_real;
  int err;

  w_assert(w_string_path_is_absolute(target),
           "get_normalized_target: path %s is not absolute\n", target->buf);
  dir_name = w_string_dirname(target);
  // Need a duplicated buffer to get terminating null character
  dir_name_buf = w_string_dup_buf(dir_name);
  file_name = w_string_basename(target);

  dir_name_real = w_realpath(dir_name_buf);
  err = errno;

  if (dir_name_real) {
    w_string_t *dir_name_real_wstr =
      w_string_new_typed(dir_name_real, W_STRING_BYTE);
    normalized_target = w_string_path_cat(dir_name_real_wstr, file_name);
    w_string_delref(dir_name_real_wstr);
    free(dir_name_real);
  }
  w_string_delref(dir_name);
  w_string_delref(file_name);
  free(dir_name_buf);

  errno = err;
  return normalized_target;
}

// Requires target to be an absolute path
static void watch_symlink_target(w_string_t *target, json_t *root_files) {
  char *watched_root = NULL, *relpath = NULL;
  w_string_t *normalized_target;

  w_assert(w_string_path_is_absolute(target),
           "watch_symlink_target: path %s is not absolute\n", target->buf);
  normalized_target = get_normalized_target(target);
  if (!normalized_target) {
    w_log(W_LOG_ERR, "watch_symlink_target: "
                     "unable to get normalized version of target `%s`; "
                     "realpath errno %d %s\n",
          target->buf, errno, strerror(errno));
    return;
  }

  watched_root = w_find_enclosing_root(normalized_target->buf, &relpath);
  if (watched_root) {
    // We are already watching a root that contains this target
    free(watched_root);
    free(relpath);
  } else {
    char *resolved, *errmsg = NULL;

    resolved = w_string_dup_buf(normalized_target);
    if (!find_project_root(root_files, resolved, &relpath)) {
      w_log(W_LOG_ERR, "watch_symlink_target: No watchable root for %s\n",
            resolved);
    } else {
      struct unlocked_watchman_root unlocked;
      bool success = w_root_resolve(resolved, true, &errmsg, &unlocked);

      if (!success) {
        w_log(W_LOG_ERR, "watch_symlink_target: unable to watch %s: %s\n",
              resolved, errmsg);
      } else {
        w_root_delref(&unlocked);
      }
      free(errmsg);
    }

    // Freeing resolved also frees rel_path
    free(resolved);
  }
  w_string_delref(normalized_target);
}

/** Given an absolute path, watch all symbolic links associated with the path.
 * Since the target of a symbolic link might contain several components that
 * are themselves symlinks, this function gets called recursively on all the
 * components of path. */
static void watch_symlinks(w_string_t *path, json_t *root_files) {
  w_string_t *dir_name, *file_name;
  char link_target_path[WATCHMAN_NAME_MAX];
  ssize_t tlen = 0;
  char *path_buf = NULL;

  // We do not currently support symlinks on Windows, so comparing path to "/"
  // is ok
  if (!path || w_string_strlen(path) == 0 ||
      w_string_equal_cstring(path, "/")) {
    return;
  }

  // Duplicate to ensure that buffer is null-terminated
  path_buf = w_string_dup_buf(path);
  w_assert(w_is_path_absolute_cstr(path_buf),
           "watch_symlinks: path %s is not absolute\n", path_buf);

  dir_name = w_string_dirname(path);
  file_name = w_string_basename(path);
  tlen = readlink(path_buf, link_target_path, sizeof(link_target_path));

  if (tlen >= (ssize_t)sizeof(link_target_path)) {
    w_log(W_LOG_ERR, "watch_symlinks: readlink(%s), symlink target is too "
                     "long: %d chars >= %d chars\n",
          path_buf, (int)tlen, (int)sizeof(link_target_path));
  } else if (tlen < 0) {
    if (errno == EINVAL) {
      // The final component of path is not a symbolic link, but other
      // components in the path might be symbolic links
      watch_symlinks(dir_name, root_files);
    } else {
      w_log(W_LOG_ERR, "watch_symlinks: readlink(%s) errno=%d %s tlen=%d\n",
            path_buf, errno, strerror(errno), (int)tlen);
    }
  } else {
    w_string_t *target =
        w_string_new_len_typed(link_target_path, tlen, W_STRING_BYTE);

    if (w_string_path_is_absolute(target)) {
      watch_symlink_target(target, root_files);
      watch_symlinks(target, root_files);
      watch_symlinks(dir_name, root_files);
    } else {
      w_string_t *absolute_target = w_string_path_cat(dir_name, target);
      watch_symlink_target(absolute_target, root_files);
      watch_symlinks(absolute_target, root_files);
      // No need to watch_symlinks(dir_name), since
      // watch_symlinks(absolute_target) will eventually have the same effect
      w_string_delref(absolute_target);
    }
    w_string_delref(target);
  }

  w_string_delref(dir_name);
  w_string_delref(file_name);
  free(path_buf);
}
#endif  // Symlink-related function definitions excluded for _WIN32

/** Process the list of observed changed symlinks and arrange to establish
 * watches for their new targets */
void process_pending_symlink_targets(struct unlocked_watchman_root *unlocked) {
#ifndef _WIN32
  struct watchman_pending_fs *p, *pending;
  json_t *root_files;
  bool enforcing;

  pending = unlocked->root->pending_symlink_targets.pending;
  if (!pending) {
    return;
  }

  root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    w_log(W_LOG_ERR,
          "watch_symlink_target: error computing root_files configuration "
          "value, consult your log file at %s for more details\n", log_name);
    return;
  }

  // It is safe to work with unlocked->root->pending_symlink_targets because
  // this collection is only ever mutated from the IO thread
  unlocked->root->pending_symlink_targets.pending = NULL;
  w_pending_coll_drain(&unlocked->root->pending_symlink_targets);
  while (pending) {
    p = pending;
    pending = p->next;
    watch_symlinks(p->path, root_files);
    w_pending_fs_free(p);
  }

  json_decref(root_files);
#else
  unused_parameter(unlocked);
#endif
}


/* vim:ts=2:sw=2:et:
 */
