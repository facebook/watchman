/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/FSDetect.h"
#include "watchman/FileSystem.h"
#include "watchman/InMemoryView.h"
#include "watchman/watchman.h"

using namespace watchman;

/* Returns true if the global config root_restrict_files is not defined or if
 * one of the files in root_restrict_files exists, false otherwise. */
static bool root_check_restrict(const char* watch_path) {
  uint32_t i;
  bool enforcing;

  auto root_restrict_files = cfg_compute_root_files(&enforcing);
  if (!root_restrict_files) {
    return true;
  }
  if (!enforcing) {
    return true;
  }

  for (i = 0; i < json_array_size(root_restrict_files); i++) {
    auto obj = json_array_get(root_restrict_files, i);
    const char* restrict_file = json_string_value(obj);
    bool rv;

    if (!restrict_file) {
      logf(
          ERR,
          "resolve_root: global config root_restrict_files "
          "element {} should be a string\n",
          i);
      continue;
    }

    auto restrict_path = folly::to<std::string>(watch_path, "/", restrict_file);
    rv = w_path_exists(restrict_path.c_str());
    if (rv)
      return true;
  }

  return false;
}

static void check_allowed_fs(const char* filename, const w_string& fs_type) {
  uint32_t i;
  const char* advice = NULL;

  // Report this to the log always, as it is helpful in understanding
  // problem reports
  logf(ERR, "path {} is on filesystem type {}\n", filename, fs_type);

  auto illegal_fstypes = cfg_get_json("illegal_fstypes");
  if (!illegal_fstypes) {
    return;
  }

  auto advice_string = cfg_get_json("illegal_fstypes_advice");
  if (advice_string) {
    advice = json_string_value(advice_string);
  }
  if (!advice) {
    advice = "relocate the dir to an allowed filesystem type";
  }

  if (!illegal_fstypes.isArray()) {
    logf(ERR, "resolve_root: global config illegal_fstypes is not an array\n");
    return;
  }

  for (i = 0; i < json_array_size(illegal_fstypes); i++) {
    auto obj = json_array_get(illegal_fstypes, i);
    const char* name = json_string_value(obj);

    if (!name) {
      logf(
          ERR,
          "resolve_root: global config illegal_fstypes "
          "element {} should be a string\n",
          i);
      continue;
    }

    if (!w_string_equal_cstring(fs_type, name)) {
      continue;
    }

    throw RootResolveError(
        "path uses the \"",
        fs_type,
        "\" filesystem "
        "and is disallowed by global config illegal_fstypes: ",
        advice);
  }
}

std::shared_ptr<watchman_root>
root_resolve(const char* filename, bool auto_watch, bool* created) {
  std::error_code realpath_err;
  std::shared_ptr<watchman_root> root;

  *created = false;

  // Sanity check that the path is absolute
  if (!w_is_path_absolute_cstr(filename)) {
    watchman::log(
        watchman::ERR,
        "resolve_root: path \"",
        filename,
        "\" must be absolute\n");
    throw RootResolveError("path \"", filename, "\" must be absolute");
  }

  if (!strcmp(filename, "/")) {
    watchman::log(watchman::ERR, "resolve_root: cannot watchman \"/\"\n");
    throw RootResolveError("cannot watch \"/\"");
  }

  w_string root_str;

  try {
    root_str = watchman::realPath(filename);
    try {
      watchman::getFileInformation(filename);
    } catch (const std::system_error& exc) {
      if (exc.code() == watchman::error_code::no_such_file_or_directory) {
        throw RootResolveError(
            "\"",
            filename,
            "\" resolved to \"",
            root_str,
            "\" but we were "
            "unable to examine \"",
            filename,
            "\" using strict "
            "case sensitive rules.  Please check "
            "each component of the path and make "
            "sure that that path exactly matches "
            "the correct case of the files on your "
            "filesystem.");
      }
      throw RootResolveError(
          "unable to lstat \"", filename, "\" %s", exc.what());
    }
  } catch (const std::system_error& exc) {
    realpath_err = exc.code();
    root_str = w_string(filename, W_STRING_BYTE);
  }

  {
    auto map = watched_roots.rlock();
    const auto& it = map->find(root_str);
    if (it != map->end()) {
      root = it->second;
    }
  }

  if (!root && realpath_err.value() != 0) {
    // Path didn't resolve and neither did the name they passed in
    throw RootResolveError(
        "realpath(", filename, ") -> ", realpath_err.message());
  }

  if (root || !auto_watch) {
    if (!root) {
      throw RootResolveError("directory ", root_str, " is not watched");
    }

    // Treat this as new activity for aging purposes; this roughly maps
    // to a client querying something about the root and should extend
    // the lifetime of the root

    // Note that this write potentially races with the read in consider_reap
    // but we're "OK" with it because the latter is performed under a write
    // lock and the worst case side effect is that we (safely) decide to reap
    // at the same instant that a new command comes in.  The reap intervals
    // are typically on the order of days.
    root->inner.last_cmd_timestamp.store(
        std::chrono::steady_clock::now(), std::memory_order_release);
    return root;
  }

  logf(DBG, "Want to watch {} -> {}\n", filename, root_str);

  auto fs_type = w_fstype(filename);
  check_allowed_fs(root_str.c_str(), fs_type);

  if (!root_check_restrict(root_str.c_str())) {
    bool enforcing;
    auto root_files = cfg_compute_root_files(&enforcing);
    auto root_files_list = cfg_pretty_print_root_files(root_files);
    throw RootResolveError(
        "Your watchman administrator has configured watchman "
        "to prevent watching path `",
        root_str,
        "`.  None of the files "
        "listed in global config root_files are "
        "present and enforce_root_files is set to true.  "
        "root_files is defined by the `",
        cfg_get_global_config_file_path(),
        "` config file and "
        "includes ",
        root_files_list,
        ".  One or more of these files must be "
        "present in order to allow a watch.  Try pulling "
        "and checking out a newer version of the project?");
  }

  root = std::make_shared<watchman_root>(root_str, fs_type);

  {
    auto wlock = watched_roots.wlock();
    auto& map = *wlock;
    auto& existing = map[root->root_path];
    if (existing) {
      // Someone beat us in this race
      root = existing;
      *created = false;
    } else {
      existing = root;
      *created = true;
    }
  }

  return root;
}

std::shared_ptr<watchman_root> w_root_resolve(
    const char* filename,
    bool auto_watch) {
  bool created = false;
  auto root = root_resolve(filename, auto_watch, &created);

  if (created) {
    try {
      root->view()->startThreads(root);
    } catch (const std::exception& e) {
      watchman::log(
          watchman::ERR,
          "w_root_resolve, while calling startThreads: ",
          e.what());
      root->cancel();
      throw;
    }
    w_state_save();
  }
  return root;
}

std::shared_ptr<watchman_root> w_root_resolve_for_client_mode(
    const char* filename) {
  bool created = false;
  auto root = root_resolve(filename, true, &created);

  if (created) {
    auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
    if (!view) {
      throw RootResolveError("client mode not available");
    }

    /* force a walk now */
    view->clientModeCrawl(root);
  }
  return root;
}

/* vim:ts=2:sw=2:et:
 */
