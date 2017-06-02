/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
using watchman::realPath;

bool w_cmd_realpath_root(json_ref& args, char** errmsg) {
  const char *path;

  if (json_array_size(args) < 2) {
    ignore_result(asprintf(errmsg, "wrong number of arguments"));
    return false;
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    ignore_result(asprintf(errmsg, "second argument must be a string"));
    return false;
  }

  try {
    auto resolved = realPath(path);
    args.array()[1] = w_string_to_json(resolved);
    return true;
  } catch (const std::exception &exc) {
    watchman::log(watchman::DBG, "w_cmd_realpath_root: path ", path,
                  " does not resolve: ", exc.what(), "\n");
    // We don't treat this as an error; the caller will subsequently
    // fail and perform their usual error handling
    return true;
  }
}
W_CAP_REG("clock-sync-timeout")

/* clock /root [options]
 * Returns the current clock value for a watched root
 * If the options contain a sync_timeout, we ensure that the repo
 * is synced up-to-date and the returned clock represents the
 * latest state.
 */
static void cmd_clock(struct watchman_client* client, const json_ref& args) {
  int sync_timeout = 0;

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
  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  if (sync_timeout &&
      !root->syncToNow(std::chrono::milliseconds(sync_timeout))) {
    send_error_response(client, "sync_timeout expired");
    return;
  }

  auto resp = make_response();
  annotate_with_clock(root, resp);

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("clock", cmd_clock, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* watch-del /root
 * Stops watching the specified root */
static void cmd_watch_delete(
    struct watchman_client* client,
    const json_ref& args) {

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch-del'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto resp = make_response();
  resp.set({{"watch-del", json_boolean(root->stopWatch())},
            {"root", w_string_to_json(root->root_path)}});
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("watch-del", cmd_watch_delete, CMD_DAEMON, w_cmd_realpath_root)

/* watch-del-all
 * Stops watching all roots */
static void cmd_watch_del_all(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();
  auto roots = w_root_stop_watch_all();
  resp.set("roots", std::move(roots));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("watch-del-all", cmd_watch_del_all, CMD_DAEMON | CMD_POISON_IMMUNE,
          NULL)

/* watch-list
 * Returns a list of watched roots */
static void cmd_watch_list(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();
  auto root_paths = w_root_watch_list_to_json();
  resp.set("roots", std::move(root_paths));
  send_and_dispose_response(client, std::move(resp));
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

    ignore_result(asprintf(&proj_path, "%s/%s", candidate_dir, root_file));
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
    if (strlen(candidate_dir) == 3 && candidate_dir[1] == ':' &&
        is_slash(candidate_dir[2])) {
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

bool find_project_root(
    const json_ref& root_files,
    char* resolved,
    char** relpath) {
  uint32_t i;

  for (i = 0; i < json_array_size(root_files); i++) {
    auto item = json_array_get(root_files, i);
    const char *name = json_string_value(item);

    if (find_file_in_dir_tree(name, resolved, relpath)) {
      return true;
    }
  }
  return false;
}

// For watch-project, take a root path string and resolve the
// containing project directory, then update the args to reflect
// that path.
// relpath will hold the path to the project dir, relative to the
// watched dir.  If it is NULL it means that the project dir is
// equivalent to the watched dir.
static char*
resolve_projpath(const json_ref& args, char** errmsg, char** relpath) {
  const char *path;
  char *resolved = NULL;
  bool res = false;
  bool enforcing;
  char *enclosing = NULL;

  *relpath = NULL;

  if (json_array_size(args) < 2) {
    ignore_result(asprintf(errmsg, "wrong number of arguments"));
    return nullptr;
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    ignore_result(asprintf(errmsg, "second argument must be a string"));
    return nullptr;
  }

  try {
    auto real = realPath(path);
    resolved = strdup(real.c_str());
  } catch (const std::exception &exc) {
    ignore_result(asprintf(errmsg,
          "resolve_projpath: path `%s`: %s", path, exc.what()));
    return nullptr;
  }

  auto root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    ignore_result(asprintf(errmsg,
          "resolve_projpath: error computing root_files configuration value, "
          "consult your log file at %s for more details", log_name));
    return nullptr;
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

  res = find_project_root(root_files, resolved, relpath);
  if (res) {
    json_array_set_new(args, 1, typed_string_to_json(resolved,
          W_STRING_BYTE));
    goto done;
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
  if (!res) {
    free(resolved);
    resolved = nullptr;
  }
  return resolved;
}

/* watch /root */
static void cmd_watch(struct watchman_client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }
  root->view()->waitUntilReadyToQuery(root).wait();

  auto resp = make_response();

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set({{"watch", w_string_to_json(root->root_path)},
              {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("watch", cmd_watch, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

static void cmd_watch_project(
    struct watchman_client* client,
    const json_ref& args) {
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
    send_error_response(client, "%s", errmsg);
    free(errmsg);
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    free(dir_to_watch);
    return;
  }

  root->view()->waitUntilReadyToQuery(root).wait();

  auto resp = make_response();

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set({{"watch", w_string_to_json(root->root_path)},
              {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  if (rel_path_from_watch) {
    resp.set(
        "relative_path",
        typed_string_to_json(rel_path_from_watch, W_STRING_BYTE));
  }
  send_and_dispose_response(client, std::move(resp));
  free(dir_to_watch);
}
W_CMD_REG("watch-project", cmd_watch_project, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
