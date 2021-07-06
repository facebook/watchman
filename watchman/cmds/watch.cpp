/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/LogConfig.h"
#include "watchman/watchman.h"

using namespace watchman;

void w_cmd_realpath_root(json_ref& args) {
  const char* path;

  if (json_array_size(args) < 2) {
    throw CommandValidationError("wrong number of arguments");
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    throw CommandValidationError(
        "second argument must be a string expressing the path to the watch");
  }

  try {
    auto resolved = realPath(path);
    args.array()[1] = w_string_to_json(resolved);
  } catch (const std::exception& exc) {
    throw CommandValidationError(
        "Could not resolve ",
        path,
        " to the canonical watch path: ",
        exc.what());
  }
}
W_CAP_REG("clock-sync-timeout")

/* Add the current clock value to the response */
static void annotate_with_clock(
    const std::shared_ptr<watchman_root>& root,
    json_ref& resp) {
  resp.set("clock", w_string_to_json(root->view()->getCurrentClockString()));
}

/* clock /root [options]
 * Returns the current clock value for a watched root
 * If the options contain a sync_timeout, we ensure that the repo
 * is synced up-to-date and the returned clock represents the
 * latest state.
 */
static void cmd_clock(struct watchman_client* client, const json_ref& args) {
  int sync_timeout = 0;

  if (json_array_size(args) == 3) {
    auto& opts = args.at(2);
    if (!opts.isObject()) {
      send_error_response(
          client, "the third argument to 'clock' must be an optional object");
      return;
    }

    auto sync = opts.get_default("sync_timeout");
    if (sync) {
      if (!sync.isInt()) {
        send_error_response(
            client,
            "the sync_timeout option passed to 'clock' must be an integer");
        return;
      }
      sync_timeout = sync.asInt();
    }
  } else if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'clock'");
    return;
  }

  /* resolve the root */
  auto root = resolveRoot(client, args);

  if (sync_timeout) {
    root->syncToNow(std::chrono::milliseconds(sync_timeout));
  }

  auto resp = make_response();
  annotate_with_clock(root, resp);

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "clock",
    cmd_clock,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
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

  auto root = resolveRoot(client, args);

  auto resp = make_response();
  resp.set(
      {{"watch-del", json_boolean(root->stopWatch())},
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
W_CMD_REG(
    "watch-del-all",
    cmd_watch_del_all,
    CMD_DAEMON | CMD_POISON_IMMUNE,
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
// not found, return false. candidate_dir is modified by this function if
// return true.
static bool find_file_in_dir_tree(
    const w_string& root_file,
    w_string_piece& candidate_dir,
    w_string_piece& relpath) {
  w_string_piece current_dir(candidate_dir);
  while (true) {
    auto projPath = w_string::pathCat({current_dir, root_file});

    if (w_path_exists(projPath.c_str())) {
      // Got a match
      relpath = w_string_piece(candidate_dir);
      if (candidate_dir.size() == current_dir.size()) {
        relpath = w_string_piece();
      } else {
        relpath.advance(current_dir.size() + 1);
        candidate_dir = current_dir;
      }
      return true;
    }

    auto parent = current_dir.dirName();
    if (parent == nullptr || parent == current_dir) {
      return false;
    }
    current_dir = parent;
  }
  return false;
}

bool find_project_root(
    const json_ref& root_files,
    w_string_piece& resolved,
    w_string_piece& relpath) {
  uint32_t i;

  for (i = 0; i < json_array_size(root_files); i++) {
    auto item = json_array_get(root_files, i);
    auto name = json_to_w_string(item);

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
static w_string resolve_projpath(const json_ref& args, w_string& relpath) {
  const char* path;
  bool enforcing;
  if (json_array_size(args) < 2) {
    throw CommandValidationError("wrong number of arguments");
  }

  path = json_string_value(json_array_get(args, 1));
  if (!path) {
    throw CommandValidationError("second argument must be a string");
  }

  auto resolved = realPath(path);

  auto root_files = cfg_compute_root_files(&enforcing);
  if (!root_files) {
    throw CommandValidationError(
        "resolve_projpath: error computing root_files configuration value, "
        "consult your log file at ",
        log_name,
        " for more details");
  }

  // See if we're requesting something in a pre-existing watch

  w_string_piece prefix;
  w_string_piece relpiece;
  if (findEnclosingRoot(resolved, prefix, relpiece)) {
    relpath = relpiece.asWString();
    resolved = prefix.asWString();
    json_array_set_new(args, 1, w_string_to_json(resolved));
    return resolved;
  }
  auto resolvedpiece = resolved.piece();
  if (find_project_root(root_files, resolvedpiece, relpiece)) {
    relpath = relpiece.asWString();
    resolved = resolvedpiece.asWString();
    json_array_set_new(args, 1, w_string_to_json(resolved));
    return resolved;
  }

  if (!enforcing) {
    // We'll use the path they originally requested
    return resolved;
  }

  // Convert root files to comma delimited string for error message
  auto root_files_list = cfg_pretty_print_root_files(root_files);

  throw CommandValidationError(
      "resolve_projpath:  None of the files listed in global config "
      "root_files are present in path `",
      path,
      "` or any of its "
      "parent directories.  root_files is defined by the "
      "`",
      cfg_get_global_config_file_path(),
      "` config file and includes ",
      root_files_list,
      ".  "
      "One or more of these files must be present in order to allow "
      "a watch. Try pulling and checking out a newer version of the project?");
}

/* watch /root */
static void cmd_watch(struct watchman_client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  auto root = resolveOrCreateRoot(client, args);
  root->view()->waitUntilReadyToQuery(root).wait();

  auto resp = make_response();

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set(
        {{"watch", w_string_to_json(root->root_path)},
         {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "watch",
    cmd_watch,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

static void cmd_watch_project(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch-project'");
    return;
  }

  w_string rel_path_from_watch;
  auto dir_to_watch = resolve_projpath(args, rel_path_from_watch);

  auto root = resolveOrCreateRoot(client, args);

  root->view()->waitUntilReadyToQuery(root).wait();

  auto resp = make_response();

  if (root->failure_reason) {
    resp.set("error", w_string_to_json(root->failure_reason));
  } else if (root->inner.cancelled) {
    resp.set(
        "error", typed_string_to_json("root was cancelled", W_STRING_UNICODE));
  } else {
    resp.set(
        {{"watch", w_string_to_json(root->root_path)},
         {"watcher", w_string_to_json(root->view()->getName())}});
  }
  add_root_warnings_to_response(resp, root);
  if (!rel_path_from_watch.empty()) {
    resp.set("relative_path", w_string_to_json(rel_path_from_watch));
  }
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "watch-project",
    cmd_watch_project,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
