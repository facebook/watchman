/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/ScopeGuard.h>
#include <memory>
#include "watchman/Errors.h"
#include "watchman/FileSystem.h"
#include "watchman/LogConfig.h"
#include "watchman/watchman.h"
#include "watchman/watchman_system.h"

using watchman::readSymbolicLink;
using watchman::realPath;

// Given a target of the form "absolute_path/filename", return
// realpath(absolute_path) + filename, where realpath(absolute_path) resolves
// all the symlinks in absolute_path.
static w_string get_normalized_target(const w_string& target) {
  int err;

  w_assert(
      w_string_path_is_absolute(target),
      "get_normalized_target: path %s is not absolute\n",
      target.c_str());

  auto dir_name = target.dirName();
  auto dir_name_real = realPath(dir_name.c_str());
  err = errno;

  if (dir_name_real) {
    auto file_name = target.baseName();
    return w_string::pathCat({dir_name_real, file_name});
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

  w_string normalized_target;
  try {
    normalized_target = get_normalized_target(target);
  } catch (const std::system_error& exc) {
    watchman::log(
        watchman::ERR,
        "watch_symlink_target: unable to get normalized version of target `",
        target,
        "`; realpath ",
        exc.what(),
        "\n");
    return;
  }

  w_string_piece relpath;
  w_string_piece watched_root;
  bool enclosing = findEnclosingRoot(normalized_target, watched_root, relpath);
  if (!enclosing) {
    w_string_piece resolved(normalized_target);

    if (!find_project_root(root_files, resolved, relpath)) {
      watchman::log(
          watchman::ERR,
          "watch_symlink_target: No watchable root for ",
          resolved,
          "\n");
    } else {
      try {
        auto root = w_root_resolve(resolved.asWString().c_str(), true);
      } catch (const std::exception& exc) {
        watchman::log(
            watchman::ERR,
            "watch_symlink_target: unable to watch ",
            resolved,
            ": ",
            exc.what(),
            "\n");
      }
    }
  }
}

/** Given an absolute path, watch all symbolic links associated with the path.
 * Since the target of a symbolic link might contain several components that
 * are themselves symlinks, this function gets called recursively on all the
 * components of path. */
static void watch_symlinks(const w_string& path, json_t* root_files) {
  w_string_piece pathPiece(path);
  auto parentPiece = pathPiece.dirName();

  if (parentPiece == pathPiece) {
    // We've reached the root of the VFS; we're either "/" on unix,
    // or something like "C:\" on windows
    return;
  }
  if (!pathPiece.pathIsAbsolute()) {
    return;
  }

  auto dir_name = path.dirName();
  auto file_name = path.baseName();

  try {
    auto target = readSymbolicLink(path.c_str());

    if (w_string_piece(target).pathIsAbsolute()) {
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
  } catch (const std::system_error& exc) {
    if (exc.code() == watchman::error_code::not_a_symlink) {
      // The final component of path is not a symbolic link, but other
      // components in the path might be symbolic links
      watch_symlinks(dir_name, root_files);
    } else {
      watchman::log(
          watchman::ERR,
          "watch_symlinks: readSymbolicLink(",
          path,
          ") ",
          exc.what(),
          "\n");
    }
  }
}

/** Process the list of observed changed symlinks and arrange to establish
 * watches for their new targets */
void watchman_root::processPendingSymlinkTargets() {
  bool enforcing;

  auto pendingLock = inner.pending_symlink_targets.lock();

  if (pendingLock->empty()) {
    return;
  }

  auto root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    watchman::log(
        watchman::ERR,
        "watch_symlink_target: error computing root_files configuration "
        "value, consult your log file at ",
        log_name,
        " for more details\n");
    return;
  }

  auto p = pendingLock->stealItems();
  while (p) {
    watch_symlinks(p->path, root_files);
    p = std::move(p->next);
  }
}

/* vim:ts=2:sw=2:et:
 */
