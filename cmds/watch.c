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
    json_array_set_new(args, 1, json_string_nocheck(resolved));
  }

  return true;
}

/* clock /root
 * Returns the current clock value for a watched root
 */
static void cmd_clock(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'clock'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  resp = make_response();
  w_root_lock(root);
  annotate_with_clock(root, resp);
  w_root_unlock(root);

  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("clock", cmd_clock, CMD_DAEMON, w_cmd_realpath_root)

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
  set_prop(resp, "root", json_string_nocheck(root->root_path->buf));
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("watch-del", cmd_watch_delete, CMD_DAEMON, w_cmd_realpath_root)

/* watch-del-all
 * Stops watching all roots */
static void cmd_watch_del_all(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  json_t *purged;
  json_t *root_paths;
  size_t  root_paths_size, i;
  unused_parameter(args);

  root_paths = w_root_watch_list_to_json();
  root_paths_size = json_array_size(root_paths);

  purged = json_array();

  for (i = 0; i < root_paths_size; i++) {
    w_root_t *root = resolve_root_or_err(client, root_paths, i, false);

    if (root && w_root_stop_watch(root)) {
      json_array_append_new(purged, json_string_nocheck(root->root_path->buf));
    }
  }

  json_decref(root_paths);

  resp = make_response();
  set_prop(resp, "purged", purged);
  send_and_dispose_response(client, resp);
}
W_CMD_REG("watch-del-all", cmd_watch_del_all, CMD_DAEMON, NULL)

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
W_CMD_REG("watch-list", cmd_watch_list, CMD_DAEMON, NULL)

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

    ignore_result(asprintf(&proj_path, "%s/%s", candidate_dir, root_file));
    rv = access(proj_path, F_OK);
    free(proj_path);

    if (rv == 0) {
      // Got a match
      if (restore_slash) {
        *relpath = restore_slash + 1;
      } else {
        *relpath = NULL;
      }
      return true;
    }

    // Walk up to the next level
    if (!strcmp(candidate_dir, "/")) {
      // Can't go any higher than this
      break;
    }

    slash = strrchr(candidate_dir, '/');
    if (restore_slash) {
      *restore_slash = '/';
    }
    if (!slash) {
      break;
    }
    restore_slash = slash;
    *slash = 0;
  }

  if (restore_slash) {
    *restore_slash = '/';
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

  // Note: cfg_compute_root_files ensures that .watchmanconfig is first
  // in the returned list of files.  This is important because it is the
  // definitive indicator for the location of the project root.
  for (i = 0; i < json_array_size(root_files); i++) {
    json_t *item = json_array_get(root_files, i);
    const char *name = json_string_value(item);

    if (find_file_in_dir_tree(name, resolved, relpath)) {
      json_array_set_new(args, 1, json_string_nocheck(resolved));
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

  w_root_lock(root);
  if (root->failure_reason) {
    set_prop(resp, "error", json_string_nocheck(root->failure_reason->buf));
  } else if (root->cancelled) {
    set_prop(resp, "error", json_string_nocheck("root was cancelled"));
  } else {
    set_prop(resp, "watch", json_string_nocheck(root->root_path->buf));
  }
  send_and_dispose_response(client, resp);
  w_root_unlock(root);
  w_root_delref(root);
}
W_CMD_REG("watch", cmd_watch, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_watch_project(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  char *dir_to_watch = NULL;
  char *rel_path_from_watch = NULL;
  char *errmsg = NULL;

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

  w_root_lock(root);
  if (root->failure_reason) {
    set_prop(resp, "error", json_string_nocheck(root->failure_reason->buf));
  } else if (root->cancelled) {
    set_prop(resp, "error", json_string_nocheck("root was cancelled"));
  } else {
    set_prop(resp, "watch", json_string_nocheck(root->root_path->buf));
  }
  if (rel_path_from_watch) {
    set_prop(resp, "relative_path",
        json_string_nocheck(rel_path_from_watch));
  }
  send_and_dispose_response(client, resp);
  w_root_unlock(root);
  w_root_delref(root);
  free(dir_to_watch);
}
W_CMD_REG("watch-project", cmd_watch_project, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
