/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static long next_root_number = 1;

static const size_t root_init_offset = offsetof(w_root_t, _init_sentinel_);

static void delete_trigger(w_ht_val_t val) {
  struct watchman_trigger_command *cmd = w_ht_val_ptr(val);

  w_trigger_command_free(cmd);
}

static const struct watchman_hash_funcs trigger_hash_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  delete_trigger
};

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

static void load_root_config(w_root_t *root, const char *path) {
  char cfgfilename[WATCHMAN_NAME_MAX];
  json_error_t err;

  snprintf(cfgfilename, sizeof(cfgfilename), "%s%c.watchmanconfig",
      path, WATCHMAN_DIR_SEP);

  if (!w_path_exists(cfgfilename)) {
    if (errno == ENOENT) {
      return;
    }
    w_log(W_LOG_ERR, "%s is not accessible: %s\n",
        cfgfilename, strerror(errno));
    return;
  }

  root->config_file = json_load_file(cfgfilename, 0, &err);
  if (!root->config_file) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
        cfgfilename, err.text);
  }
}

static void apply_ignore_configuration(w_root_t *root) {
  w_string_t *name;
  w_string_t *fullname;
  uint8_t i;
  json_t *ignores;

  ignores = cfg_get_json(root, "ignore_dirs");
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

    name = json_to_w_string(jignore);
    fullname = w_string_path_cat(root->root_path, name);
    w_ignore_addstr(&root->ignore, fullname, false);
    w_log(W_LOG_DBG, "ignoring %.*s recursively\n",
        fullname->len, fullname->buf);
    w_string_delref(fullname);
  }
}

// internal initialization for root
bool w_root_init(w_root_t *root, char **errmsg) {
  struct watchman_dir_handle *osdir;

  memset((char *)root + root_init_offset, 0,
         sizeof(w_root_t) - root_init_offset);

  osdir = w_dir_open(root->root_path->buf);
  if (!osdir) {
    ignore_result(asprintf(errmsg, "failed to opendir(%s): %s",
          root->root_path->buf,
          strerror(errno)));
    return false;
  }
  w_dir_close(osdir);

  if (!w_watcher_init(root, errmsg)) {
    return false;
  }

  root->number = __sync_fetch_and_add(&next_root_number, 1);

  root->cursors = w_ht_new(2, &w_ht_string_funcs);
  root->suffixes = w_ht_new(2, &w_ht_string_funcs);
  root->ticks = 1;

  // "manually" populate the initial dir, as the dir resolver will
  // try to find its parent and we don't want it to for the root
  root->root_dir = calloc(1, sizeof(*root->root_dir));
  root->root_dir->name = root->root_path;
  w_string_addref(root->root_dir->name);

  time(&root->last_cmd_timestamp);

  return root;
}

w_root_t *w_root_new(const char *path, char **errmsg) {
  w_root_t *root = calloc(1, sizeof(*root));

  assert(root != NULL);

  root->refcnt = 1;
  w_refcnt_add(&live_roots);
  pthread_rwlock_init(&root->lock, NULL);

  root->case_sensitive = is_case_sensitive_filesystem(path);

  w_pending_coll_init(&root->pending);
  w_pending_coll_init(&root->pending_symlink_targets);
  root->root_path = w_string_new_typed(path, W_STRING_BYTE);
  root->commands = w_ht_new(2, &trigger_hash_funcs);
  root->query_cookies = w_ht_new(2, &w_ht_string_funcs);
  w_ignore_init(&root->ignore);

  load_root_config(root, path);
  root->trigger_settle = (int)cfg_get_int(
      root, "settle", DEFAULT_SETTLE_PERIOD);
  root->gc_age = (int)cfg_get_int(root, "gc_age_seconds", DEFAULT_GC_AGE);
  root->gc_interval = (int)cfg_get_int(root, "gc_interval_seconds",
      DEFAULT_GC_INTERVAL);
  root->idle_reap_age = (int)cfg_get_int(root, "idle_reap_age_seconds",
      DEFAULT_REAP_AGE);

  apply_ignore_configuration(root);

  if (!apply_ignore_vcs_configuration(root, errmsg)) {
    w_root_delref_raw(root);
    return NULL;
  }

  if (!w_root_init(root, errmsg)) {
    w_root_delref_raw(root);
    return NULL;
  }
  return root;
}

void w_root_teardown(w_root_t *root) {
  struct watchman_file *file;

  if (root->watcher_ops) {
    root->watcher_ops->root_dtor(root);
  }

  if (root->root_dir) {
    delete_dir(root->root_dir);
  }
  w_pending_coll_drain(&root->pending);
  w_pending_coll_drain(&root->pending_symlink_targets);

  while (root->latest_file) {
    file = root->latest_file;
    root->latest_file = file->next;
    free_file_node(root, file);
  }

  if (root->cursors) {
    w_ht_free(root->cursors);
    root->cursors = NULL;
  }
  if (root->suffixes) {
    w_ht_free(root->suffixes);
    root->suffixes = NULL;
  }
}

void w_root_addref(w_root_t *root) {
  w_refcnt_add(&root->refcnt);
}

void w_root_delref(struct unlocked_watchman_root *unlocked) {
  if (!unlocked->root) {
    w_log(W_LOG_FATAL, "already released root passed to w_root_delref");
  }
  w_root_delref_raw(unlocked->root);
  unlocked->root = NULL;
}

void w_root_delref_raw(w_root_t *root) {
  if (!w_refcnt_del(&root->refcnt)) return;

  w_log(W_LOG_DBG, "root: final ref on %s\n",
      root->root_path->buf);
  w_cancel_subscriptions_for_root(root);

  w_root_teardown(root);

  pthread_rwlock_destroy(&root->lock);
  w_string_delref(root->root_path);
  w_ignore_destroy(&root->ignore);
  w_ht_free(root->commands);
  w_ht_free(root->query_cookies);

  if (root->config_file) {
    json_decref(root->config_file);
  }

  if (root->last_recrawl_reason) {
    w_string_delref(root->last_recrawl_reason);
  }
  if (root->failure_reason) {
    w_string_delref(root->failure_reason);
  }
  if (root->warning) {
    w_string_delref(root->warning);
  }

  if (root->query_cookie_dir) {
    w_string_delref(root->query_cookie_dir);
  }
  if (root->query_cookie_prefix) {
    w_string_delref(root->query_cookie_prefix);
  }
  w_pending_coll_destroy(&root->pending);
  w_pending_coll_destroy(&root->pending_symlink_targets);

  free(root);
  w_refcnt_del(&live_roots);
}

/* vim:ts=2:sw=2:et:
 */
