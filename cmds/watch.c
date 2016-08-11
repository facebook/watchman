/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool w_cmd_realpath_root(json_t *args, char **errmsg)
{
  const char *path;
  char *resolved;

  if (json_array_size(args) < 2) {
    ignore_result(asprintf(errmsg, "wrong number of arguments"));
    return false;
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    ignore_result(asprintf(errmsg, "second argument must be a string"));
    return false;
  }

  resolved = w_realpath(path);
  if (resolved) {
    json_array_set_new(args, 1, typed_string_to_json(resolved,
          W_STRING_BYTE));
    free(resolved);
  }

  return true;
}
W_CAP_REG("clock-sync-timeout")

/* clock /root [options]
 * Returns the current clock value for a watched root
 * If the options contain a sync_timeout, we ensure that the repo
 * is synced up-to-date and the returned clock represents the
 * latest state.
 */
static void cmd_clock(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  int sync_timeout = 0;
  struct write_locked_watchman_root lock;

  if (json_array_size(args) == 3) {
    const char *ignored;
    if (0 != json_unpack(args, "[s, s, {s?:i*}]",
                         &ignored, &ignored,
                         "sync_timeout", &sync_timeout)) {
      send_error_response(client, "malformated argument list for 'clock'");
      return;
    }
  } else if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'clock'");
    return;
  }

  /* resolve the root */
  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  if (sync_timeout && !w_root_sync_to_now(root, sync_timeout)) {
    send_error_response(client, "sync_timeout expired");
    w_root_delref(root);
    return;
  }

  resp = make_response();
  w_root_lock(&root, "clock", &lock);
  annotate_with_clock(lock.root, resp);
  root = w_root_unlock(&lock);

  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("clock", cmd_clock, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* watch-del /root
 * Stops watching the specified root */
static void cmd_watch_delete(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch-del'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  resp = make_response();
  set_prop(resp, "watch-del", json_boolean(w_root_stop_watch(root)));
  set_prop(resp, "root", w_string_to_json(root->root_path));
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("watch-del", cmd_watch_delete, CMD_DAEMON, w_cmd_realpath_root)

/* watch-del-all
 * Stops watching all roots */
static void cmd_watch_del_all(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  json_t *roots;
  unused_parameter(args);

  resp = make_response();
  roots = w_root_stop_watch_all();
  set_prop(resp, "roots", roots);
  send_and_dispose_response(client, resp);
}
W_CMD_REG("watch-del-all", cmd_watch_del_all, CMD_DAEMON | CMD_POISON_IMMUNE,
          NULL)

/* watch-list
 * Returns a list of watched roots */
static void cmd_watch_list(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  json_t *root_paths;
  unused_parameter(args);

  resp = make_response();
  root_paths = w_root_watch_list_to_json();
  set_prop(resp, "roots", root_paths);
  send_and_dispose_response(client, resp);
}
W_CMD_REG("watch-list", cmd_watch_list, CMD_DAEMON | CMD_ALLOW_ANY_USER, NULL)

// For each directory component in candidate_dir to the root of the filesystem,
// look for root_file.  If root_file is present, update relpath to reflect the
// relative path to the original value of candidate_dir and return true.  If
// not found, return false.  candidate_dir is modified by this function; it
// will poke a NUL char in to separate the resolved candidate_dir from the
// relpath.  If we return false, the original candidate_dir value is restored.
// If we return true, then we may have poked the string to separate the two
// components.
static bool find_file_in_dir_tree(const char *root_file, char *candidate_dir,
    char **relpath) {
  char *restore_slash = NULL;

  while (true) {
    char *slash;
    char *proj_path;
    int rv;

    ignore_result(asprintf(&proj_path, "%s%c%s", candidate_dir,
          WATCHMAN_DIR_SEP, root_file));
    rv = w_path_exists(proj_path);
    free(proj_path);

    if (rv) {
      // Got a match
      if (restore_slash) {
        *relpath = restore_slash + 1;
      } else {
        *relpath = NULL;
      }
      return true;
    }

    // Walk up to the next level
#ifdef _WIN32
    if (strlen(candidate_dir) == 3 &&
        candidate_dir[1] == ':' && candidate_dir[2] == '\\') {
      // Drive letter; is a root
      break;
    }
    if (strlen(candidate_dir) <= 2) {
      // Effectively a root
      break;
    }
#else
    if (!strcmp(candidate_dir, "/")) {
      // Can't go any higher than this
      break;
    }
#endif


    slash = strrchr(candidate_dir, WATCHMAN_DIR_SEP);
    if (restore_slash) {
      *restore_slash = WATCHMAN_DIR_SEP;
    }
    if (!slash) {
      break;
    }
    restore_slash = slash;
    *slash = 0;
  }

  if (restore_slash) {
    *restore_slash = WATCHMAN_DIR_SEP;
  }
  *relpath = NULL;
  return false;
}

// For watch-project, take a root path string and resolve the
// containing project directory, then update the args to reflect
// that path.
// relpath will hold the path to the project dir, relative to the
// watched dir.  If it is NULL it means that the project dir is
// equivalent to the watched dir.
static char *resolve_projpath(json_t *args, char **errmsg, char **relpath)
{
  const char *path;
  char *resolved = NULL;
  bool res = false;
  json_t *root_files;
  uint32_t i;
  bool enforcing;
  char *enclosing = NULL;

  *relpath = NULL;

  if (json_array_size(args) < 2) {
    ignore_result(asprintf(errmsg, "wrong number of arguments"));
    return false;
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    ignore_result(asprintf(errmsg, "second argument must be a string"));
    return false;
  }

  resolved = w_realpath(path);
  if (!resolved) {
    ignore_result(asprintf(errmsg,
          "resolve_projpath: path `%s` does not exist", path));
    return false;
  }

  root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    ignore_result(asprintf(errmsg,
          "resolve_projpath: error computing root_files configuration value, "
          "consult your log file at %s for more details", log_name));
    return false;
  }

  // See if we're requesting something in a pre-existing watch
  enclosing = w_find_enclosing_root(resolved, relpath);
  if (enclosing) {
    free(resolved);
    resolved = enclosing;
    json_array_set_new(args, 1, typed_string_to_json(resolved,
          W_STRING_BYTE));
    res = true;
    goto done;
  }

  // Note: cfg_compute_root_files ensures that .watchmanconfig is first
  // in the returned list of files.  This is important because it is the
  // definitive indicator for the location of the project root.
  for (i = 0; i < json_array_size(root_files); i++) {
    json_t *item = json_array_get(root_files, i);
    const char *name = json_string_value(item);

    if (find_file_in_dir_tree(name, resolved, relpath)) {
      json_array_set_new(args, 1, typed_string_to_json(resolved,
            W_STRING_BYTE));
      res = true;
      goto done;
    }
  }

  if (!enforcing) {
    // We'll use the path they originally requested
    res = true;
    goto done;
  }

  ignore_result(asprintf(errmsg,
      "resolve_projpath: none of the files listed in global config "
      "root_files are present in path `%s` or any of its "
      "parent directories", path));

done:
  json_decref(root_files);
  if (!res) {
    free(resolved);
    resolved = NULL;
  }
  return resolved;
}

/* watch /root */
static void cmd_watch(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  struct write_locked_watchman_root lock;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  resp = make_response();

  w_root_lock(&root, "watch", &lock);
  if (lock.root->failure_reason) {
    set_prop(resp, "error", w_string_to_json(root->failure_reason));
  } else if (lock.root->cancelled) {
    set_unicode_prop(resp, "error", "root was cancelled");
  } else {
    set_prop(resp, "watch", w_string_to_json(lock.root->root_path));
    set_unicode_prop(resp, "watcher", lock.root->watcher_ops->name);
  }
  add_root_warnings_to_response(resp, lock.root);
  send_and_dispose_response(client, resp);
  root = w_root_unlock(&lock);
  w_root_delref(root);
}
W_CMD_REG("watch", cmd_watch, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

static void cmd_watch_project(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  char *dir_to_watch = NULL;
  char *rel_path_from_watch = NULL;
  char *errmsg = NULL;
  struct write_locked_watchman_root lock;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch-project'");
    return;
  }

  // Implementation note: rel_path_from_watch is stored in the same
  // memory buffer as dir_to_watch; free()ing dir_to_watch will also
  // free rel_path_from_watch
  dir_to_watch = resolve_projpath(args, &errmsg, &rel_path_from_watch);
  if (!dir_to_watch) {
    send_error_response(client, errmsg);
    free(errmsg);
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    free(dir_to_watch);
    return;
  }

  resp = make_response();

  w_root_lock(&root, "watch-project", &lock);
  if (lock.root->failure_reason) {
    set_prop(resp, "error", w_string_to_json(lock.root->failure_reason));
  } else if (lock.root->cancelled) {
    set_unicode_prop(resp, "error", "root was cancelled");
  } else {
    set_prop(resp, "watch", w_string_to_json(lock.root->root_path));
    set_unicode_prop(resp, "watcher", lock.root->watcher_ops->name);
  }
  add_root_warnings_to_response(resp, lock.root);
  if (rel_path_from_watch) {
    set_bytestring_prop(resp, "relative_path",rel_path_from_watch);
  }
  send_and_dispose_response(client, resp);
  root = w_root_unlock(&lock);
  w_root_delref(root);
  free(dir_to_watch);
}
W_CMD_REG("watch-project", cmd_watch_project, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
