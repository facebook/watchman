/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

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

    ignore_result(asprintf(&restrict_path, "%s%c%s", watch_path,
          WATCHMAN_DIR_SEP, restrict_file));
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
    json_t *obj = json_array_get(illegal_fstypes, i);
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

bool root_resolve(const char *filename, bool auto_watch, bool *created,
                  char **errmsg, struct unlocked_watchman_root *unlocked) {
  struct watchman_root* root = nullptr;
  char *watch_path;
  int realpath_err;

  *created = false;
  unlocked->root = NULL;

  // Sanity check that the path is absolute
  if (!w_is_path_absolute_cstr(filename)) {
    ignore_result(asprintf(errmsg, "path \"%s\" must be absolute", filename));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return false;
  }

  if (!strcmp(filename, "/")) {
    ignore_result(asprintf(errmsg, "cannot watch \"/\""));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return false;
  }

  watch_path = w_realpath(filename);
  realpath_err = errno;

  if (!watch_path) {
    watch_path = (char*)filename;
  }

  w_string root_str(watch_path, W_STRING_BYTE);

  {
    auto map = watched_roots.rlock();
    const auto& it = map->find(root_str);
    if (it != map->end()) {
      root = it->second;
      w_root_addref(root);
    }
  }

  if (!root && watch_path == filename) {
    // Path didn't resolve and neither did the name they passed in
    ignore_result(asprintf(errmsg,
          "realpath(%s) -> %s", filename, strerror(realpath_err)));
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    return false;
  }

  if (root || !auto_watch) {
    if (!root) {
      ignore_result(
          asprintf(errmsg, "directory %s is not watched", watch_path));
      w_log(W_LOG_DBG, "resolve_root: %s\n", *errmsg);
    }
    if (watch_path != filename) {
      free(watch_path);
    }

    if (!root) {
      return false;
    }

    // Treat this as new activity for aging purposes; this roughly maps
    // to a client querying something about the root and should extend
    // the lifetime of the root

    unlocked->root = root;
    // Note that this write potentially races with the read in consider_reap
    // but we're "OK" with it because the latter is performed under a write
    // lock and the worst case side effect is that we (safely) decide to reap
    // at the same instant that a new command comes in.  The reap intervals
    // are typically on the order of days.
    time(&unlocked->root->inner.last_cmd_timestamp);
    // caller owns a ref
    return true;
  }

  w_log(W_LOG_DBG, "Want to watch %s -> %s\n", filename, watch_path);

  if (!check_allowed_fs(watch_path, errmsg)) {
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    if (watch_path != filename) {
      free(watch_path);
    }
    return false;
  }

  if (!root_check_restrict(watch_path)) {
    ignore_result(
        asprintf(errmsg, "Your watchman administrator has configured watchman "
                         "to prevent watching this path.  None of the files "
                         "listed in global config root_files are "
                         "present and enforce_root_files is set to true"));
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    if (watch_path != filename) {
      free(watch_path);
    }
    return false;
  }

  // created with 1 ref
  root = w_root_new(watch_path, errmsg);

  if (watch_path != filename) {
    free(watch_path);
  }

  if (!root) {
    return false;
  }

  {
    auto wlock = watched_roots.wlock();
    auto& map = *wlock;
    auto& existing = map[root->root_path];
    if (existing) {
      // Someone beat us in this race
      w_root_addref(existing);
      w_root_delref_raw(root);
      root = existing;
      *created = false;
    } else {
      existing = root;
      // map owns a ref
      w_root_addref(root);
      *created = true;
    }
  }

  // caller owns 1 ref
  unlocked->root = root;
  return true;
}

bool w_root_resolve(const char *filename, bool auto_watch, char **errmsg,
                    struct unlocked_watchman_root *unlocked) {
  bool created = false;
  if (!root_resolve(filename, auto_watch, &created, errmsg, unlocked)) {
    return false;
  }
  if (created) {
    if (!root_start(unlocked->root, errmsg)) {
      w_root_cancel(unlocked->root);
      w_root_delref(unlocked);
      return false;
    }
    w_state_save();
  }
  return true;
}

bool w_root_resolve_for_client_mode(const char *filename, char **errmsg,
                                    struct unlocked_watchman_root *unlocked) {
  bool created = false;

  if (!root_resolve(filename, true, &created, errmsg, unlocked)) {
    return false;
  }

  if (created) {
    auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(
        unlocked->root->inner.view);
    if (!view) {
      *errmsg = strdup("client mode not available");
      return false;
    }

    /* force a walk now */
    view->clientModeCrawl(unlocked);
  }
  return true;
}


/* vim:ts=2:sw=2:et:
 */
