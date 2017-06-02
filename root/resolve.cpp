/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
#include "FileSystem.h"
#include "watchman_error_category.h"

/* Returns true if the global config root_restrict_files is not defined or if
 * one of the files in root_restrict_files exists, false otherwise. */
static bool root_check_restrict(const char *watch_path) {
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
    const char *restrict_file = json_string_value(obj);
    char *restrict_path;
    bool rv;

    if (!restrict_file) {
      w_log(W_LOG_ERR, "resolve_root: global config root_restrict_files "
            "element %" PRIu32 " should be a string\n", i);
      continue;
    }

    ignore_result(asprintf(&restrict_path, "%s/%s", watch_path, restrict_file));
    rv = w_path_exists(restrict_path);
    free(restrict_path);
    if (rv)
      return true;
  }

  return false;
}

static bool check_allowed_fs(const char *filename, char **errmsg) {
  auto fs_type = w_fstype(filename);
  json_t *illegal_fstypes = NULL;
  json_t *advice_string;
  uint32_t i;
  const char *advice = NULL;

  // Report this to the log always, as it is helpful in understanding
  // problem reports
  w_log(
      W_LOG_ERR,
      "path %s is on filesystem type %s\n",
      filename,
      fs_type.c_str());

  illegal_fstypes = cfg_get_json("illegal_fstypes");
  if (!illegal_fstypes) {
    return true;
  }

  advice_string = cfg_get_json("illegal_fstypes_advice");
  if (advice_string) {
    advice = json_string_value(advice_string);
  }
  if (!advice) {
    advice = "relocate the dir to an allowed filesystem type";
  }

  if (!json_is_array(illegal_fstypes)) {
    w_log(W_LOG_ERR,
          "resolve_root: global config illegal_fstypes is not an array\n");
    return true;
  }

  for (i = 0; i < json_array_size(illegal_fstypes); i++) {
    auto obj = json_array_get(illegal_fstypes, i);
    const char *name = json_string_value(obj);

    if (!name) {
      w_log(W_LOG_ERR, "resolve_root: global config illegal_fstypes "
            "element %" PRIu32 " should be a string\n", i);
      continue;
    }

    if (!w_string_equal_cstring(fs_type, name)) {
      continue;
    }

    ignore_result(asprintf(
        errmsg,
        "path uses the \"%s\" filesystem "
        "and is disallowed by global config illegal_fstypes: %s",
        fs_type.c_str(),
        advice));

    return false;
  }

  return true;
}

std::shared_ptr<w_root_t> root_resolve(
    const char* filename,
    bool auto_watch,
    bool* created,
    char** errmsg) {
  std::error_code realpath_err;
  std::shared_ptr<w_root_t> root;

  *created = false;

  // Sanity check that the path is absolute
  if (!w_is_path_absolute_cstr(filename)) {
    ignore_result(asprintf(errmsg, "path \"%s\" must be absolute", filename));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return nullptr;
  }

  if (!strcmp(filename, "/")) {
    ignore_result(asprintf(errmsg, "cannot watch \"/\""));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return nullptr;
  }

  w_string root_str;

  try {
    root_str = watchman::realPath(filename);
    try {
      watchman::getFileInformation(filename);
    } catch (const std::system_error& exc) {
      if (exc.code() == watchman::error_code::no_such_file_or_directory) {
        ignore_result(asprintf(errmsg, "\"%s\" resolved to \"%s\" but we were "
                                       "unable to examine \"%s\" using strict "
                                       "case sensitive rules.  Please check "
                                       "each component of the path and make "
                                       "sure that that path exactly matches "
                                       "the correct case of the files on your "
                                       "filesystem.",
                               filename, root_str.c_str(), filename));
        w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
        return nullptr;
      }
      ignore_result(
          asprintf(errmsg, "unable to lstat \"%s\" %s", filename, exc.what()));
      w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
      return nullptr;
    }
  } catch (const std::system_error &exc) {
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
    ignore_result(asprintf(errmsg, "realpath(%s) -> %s", filename,
                           realpath_err.message().c_str()));
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    return nullptr;
  }

  if (root || !auto_watch) {
    if (!root) {
      ignore_result(
          asprintf(errmsg, "directory %s is not watched", root_str.c_str()));
      w_log(W_LOG_DBG, "resolve_root: %s\n", *errmsg);
    }

    if (!root) {
      return nullptr;
    }

    // Treat this as new activity for aging purposes; this roughly maps
    // to a client querying something about the root and should extend
    // the lifetime of the root

    // Note that this write potentially races with the read in consider_reap
    // but we're "OK" with it because the latter is performed under a write
    // lock and the worst case side effect is that we (safely) decide to reap
    // at the same instant that a new command comes in.  The reap intervals
    // are typically on the order of days.
    time(&root->inner.last_cmd_timestamp);
    return root;
  }

  w_log(W_LOG_DBG, "Want to watch %s -> %s\n", filename, root_str.c_str());

  if (!check_allowed_fs(root_str.c_str(), errmsg)) {
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    return nullptr;
  }

  if (!root_check_restrict(root_str.c_str())) {
    ignore_result(
        asprintf(errmsg, "Your watchman administrator has configured watchman "
                         "to prevent watching this path.  None of the files "
                         "listed in global config root_files are "
                         "present and enforce_root_files is set to true"));
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    return nullptr;
  }

  // created with 1 ref
  try {
    root = std::make_shared<w_root_t>(root_str);
  } catch (const std::exception& e) {
    watchman::log(watchman::ERR, "while making a new root: ", e.what());
    *errmsg = strdup(e.what());
  }

  if (!root) {
    return nullptr;
  }

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

std::shared_ptr<w_root_t>
w_root_resolve(const char* filename, bool auto_watch, char** errmsg) {
  bool created = false;
  auto root = root_resolve(filename, auto_watch, &created, errmsg);

  if (created) {
    try {
      root->view()->startThreads(root);
    } catch (const std::exception& e) {
      watchman::log(
          watchman::ERR,
          "w_root_resolve, while calling startThreads: ",
          e.what());
      *errmsg = strdup(e.what());
      root->cancel();
      return nullptr;
    }
    w_state_save();
  }
  return root;
}

std::shared_ptr<w_root_t> w_root_resolve_for_client_mode(
    const char* filename,
    char** errmsg) {
  bool created = false;
  auto root = root_resolve(filename, true, &created, errmsg);

  if (created) {
    auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
    if (!view) {
      *errmsg = strdup("client mode not available");
      return nullptr;
    }

    /* force a walk now */
    view->clientModeCrawl(root);
  }
  return root;
}

/* vim:ts=2:sw=2:et:
 */
