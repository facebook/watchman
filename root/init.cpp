/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "InMemoryView.h"
#include "make_unique.h"

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static std::atomic<long> next_root_number{1};

static bool is_case_sensitive_filesystem(const char *path) {
#ifdef __APPLE__
  return pathconf(path, _PC_CASE_SENSITIVE);
#elif defined(_WIN32)
  unused_parameter(path);
  return false;
#else
  unused_parameter(path);
  return true;
#endif
}

static json_ref load_root_config(const char* path) {
  char cfgfilename[WATCHMAN_NAME_MAX];
  json_error_t err;

  snprintf(cfgfilename, sizeof(cfgfilename), "%s%c.watchmanconfig",
      path, WATCHMAN_DIR_SEP);

  if (!w_path_exists(cfgfilename)) {
    if (errno == ENOENT) {
      return nullptr;
    }
    w_log(W_LOG_ERR, "%s is not accessible: %s\n",
        cfgfilename, strerror(errno));
    return nullptr;
  }

  auto res = json_load_file(cfgfilename, 0, &err);
  if (!res) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
        cfgfilename, err.text);
  }
  return res;
}

static void apply_ignore_configuration(w_root_t *root) {
  uint8_t i;
  json_t *ignores;

  ignores = root->config.get("ignore_dirs");
  if (!ignores) {
    return;
  }
  if (!json_is_array(ignores)) {
    w_log(W_LOG_ERR, "ignore_dirs must be an array of strings\n");
    return;
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    const json_t *jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      w_log(W_LOG_ERR, "ignore_dirs must be an array of strings\n");
      continue;
    }

    auto name = json_to_w_string(jignore);
    auto fullname = w_string::pathCat({root->root_path, name});
    root->ignore.add(fullname, false);
    w_log(W_LOG_DBG, "ignoring %s recursively\n", fullname.c_str());
  }
}

// internal initialization for root
bool w_root_init(w_root_t *root, char **errmsg) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(root->root_path.c_str());
  if (!osdir) {
    ignore_result(asprintf(errmsg, "failed to opendir(%s): %s",
          root->root_path.c_str(),
          strerror(errno)));
    return false;
  }
  w_dir_close(osdir);

  if (!w_watcher_init(root, errmsg)) {
    return false;
  }

  root->inner.number = next_root_number++;

  time(&root->inner.last_cmd_timestamp);

  return root;
}

w_root_t *w_root_new(const char *path, char **errmsg) {
  auto root = new w_root_t(w_string(path, W_STRING_BYTE));

  ++live_roots;
  pthread_rwlock_init(&root->lock, NULL);

  root->case_sensitive = is_case_sensitive_filesystem(path);

  root->trigger_settle =
      int(root->config.getInt("settle", DEFAULT_SETTLE_PERIOD));
  root->gc_age = int(root->config.getInt("gc_age_seconds", DEFAULT_GC_AGE));
  root->gc_interval =
      int(root->config.getInt("gc_interval_seconds", DEFAULT_GC_INTERVAL));
  root->idle_reap_age =
      int(root->config.getInt("idle_reap_age_seconds", DEFAULT_REAP_AGE));

  apply_ignore_configuration(root);

  if (!apply_ignore_vcs_configuration(root, errmsg)) {
    w_root_delref_raw(root);
    return nullptr;
  }

  if (!w_root_init(root, errmsg)) {
    w_root_delref_raw(root);
    return nullptr;
  }
  return root;
}

void w_root_teardown(w_root_t *root) {
  // Placement delete and then new to re-init the storage.
  // We can't just delete because we need to leave things
  // in a well defined state for when we subsequently
  // delete the containing root (that will call the Inner
  // destructor).
  root->inner.~Inner();
  new (&root->inner)
      watchman_root::Inner(root->root_path, root->cookies, root->config);
}

watchman_root::Inner::Inner(
    const w_string& root_path,
    watchman::CookieSync& cookies,
    Configuration& config)
    : view(std::make_shared<watchman::InMemoryView>(
          root_path,
          cookies,
          config)) {}

watchman_root::Inner::~Inner() {}

void w_root_addref(w_root_t *root) {
  ++root->refcnt;
}

void w_root_delref(struct unlocked_watchman_root *unlocked) {
  if (!unlocked->root) {
    w_log(W_LOG_FATAL, "already released root passed to w_root_delref");
  }
  w_root_delref_raw(unlocked->root);
  unlocked->root = NULL;
}

void w_root_delref_raw(w_root_t *root) {
  if (--root->refcnt != 0) {
    return;
  }
  delete root;
}

watchman_root::watchman_root(const w_string& root_path)
    : root_path(root_path),
      cookies(root_path),
      config_file(load_root_config(root_path.c_str())),
      config(config_file),
      inner(root_path, cookies, config) {}

watchman_root::~watchman_root() {
  w_log(W_LOG_DBG, "root: final ref on %s\n", root_path.c_str());
  w_cancel_subscriptions_for_root(this);

  pthread_rwlock_destroy(&lock);

  --live_roots;
}

/* vim:ts=2:sw=2:et:
 */
