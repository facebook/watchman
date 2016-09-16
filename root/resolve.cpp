/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Returns true if the global config root_restrict_files is not defined or if
 * one of the files in root_restrict_files exists, false otherwise. */
static bool root_check_restrict(const char *watch_path) {
  json_t *root_restrict_files = NULL;
  uint32_t i;
  bool enforcing;

  root_restrict_files = cfg_compute_root_files(&enforcing);
  if (!root_restrict_files) {
    return true;
  }
  if (!enforcing) {
    json_decref(root_restrict_files);
    return true;
  }

  for (i = 0; i < json_array_size(root_restrict_files); i++) {
    json_t *obj = json_array_get(root_restrict_files, i);
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
  w_string_t *fs_type = w_fstype(filename);
  json_t *illegal_fstypes = NULL;
  json_t *advice_string;
  uint32_t i;
  const char *advice = NULL;

  // Report this to the log always, as it is helpful in understanding
  // problem reports
  w_log(W_LOG_ERR, "path %s is on filesystem type %.*s\n",
      filename, fs_type->len, fs_type->buf);

  illegal_fstypes = cfg_get_json(NULL, "illegal_fstypes");
  if (!illegal_fstypes) {
    w_string_delref(fs_type);
    return true;
  }

  advice_string = cfg_get_json(NULL, "illegal_fstypes_advice");
  if (advice_string) {
    advice = json_string_value(advice_string);
  }
  if (!advice) {
    advice = "relocate the dir to an allowed filesystem type";
  }

  if (!json_is_array(illegal_fstypes)) {
    w_log(W_LOG_ERR,
          "resolve_root: global config illegal_fstypes is not an array\n");
    w_string_delref(fs_type);
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

    ignore_result(asprintf(errmsg,
      "path uses the \"%.*s\" filesystem "
      "and is disallowed by global config illegal_fstypes: %s",
      fs_type->len, fs_type->buf, advice));

    w_string_delref(fs_type);
    return false;
  }

  w_string_delref(fs_type);
  return true;
}

bool root_resolve(const char *filename, bool auto_watch, bool *created,
                  char **errmsg, struct unlocked_watchman_root *unlocked) {
  struct watchman_root *root = NULL, *existing = NULL;
  w_ht_val_t root_val;
  char *watch_path;
  w_string_t *root_str;
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

  root_str = w_string_new_typed(watch_path, W_STRING_BYTE);
  pthread_mutex_lock(&watch_list_lock);
  // This will addref if it returns root
  if (w_ht_lookup(watched_roots, w_ht_ptr_val(root_str), &root_val, true)) {
    root = (w_root_t*)w_ht_val_ptr(root_val);
  }
  pthread_mutex_unlock(&watch_list_lock);
  w_string_delref(root_str);

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

  pthread_mutex_lock(&watch_list_lock);
  existing = (w_root_t*)w_ht_val_ptr(
      w_ht_get(watched_roots, w_ht_ptr_val(root->root_path)));
  if (existing) {
    // Someone beat us in this race
    w_root_addref(existing);
    w_root_delref_raw(root);
    root = existing;
    *created = false;
  } else {
    // adds 1 ref
    w_ht_set(watched_roots, w_ht_ptr_val(root->root_path), w_ht_ptr_val(root));
    *created = true;
  }
  pthread_mutex_unlock(&watch_list_lock);

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
    struct timeval start;
    struct watchman_pending_collection pending;
    struct write_locked_watchman_root lock;

    w_pending_coll_init(&pending);

    /* force a walk now */
    gettimeofday(&start, NULL);
    w_root_lock(unlocked, "w_root_resolve_for_client_mode", &lock);
    w_pending_coll_add(&lock.root->pending, lock.root->root_path,
        start, W_PENDING_RECURSIVE);
    while (w_root_process_pending(&lock, &pending, true)) {
      // Note that we don't need a two-level loop (as we do in the main
      // watcher-enabled mode) in client mode as we are not using a
      // watcher in this situation.
      ;
    }
    w_root_unlock(&lock, unlocked);

    w_pending_coll_destroy(&pending);
  }
  return true;
}


/* vim:ts=2:sw=2:et:
 */
