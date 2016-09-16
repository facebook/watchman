/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

w_ht_t *watched_roots = NULL;
volatile long live_roots = 0;
pthread_mutex_t watch_list_lock = PTHREAD_MUTEX_INITIALIZER;

static w_ht_val_t root_copy_val(w_ht_val_t val) {
  auto root = (w_root_t *)w_ht_val_ptr(val);

  w_root_addref(root);

  return val;
}

static void root_del_val(w_ht_val_t val) {
  auto root = (w_root_t *)w_ht_val_ptr(val);

  w_root_delref_raw(root);
}

static const struct watchman_hash_funcs root_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  root_copy_val,
  root_del_val
};

void watchman_watcher_init(void) {
  watched_roots = w_ht_new(4, &root_funcs);
}

bool remove_root_from_watched(
    w_root_t *root /* don't care about locked state */) {
  bool removed = false;
  pthread_mutex_lock(&watch_list_lock);
  // it's possible that the root has already been removed and replaced with
  // another, so make sure we're removing the right object
  if (w_ht_val_ptr(w_ht_get(watched_roots, w_ht_ptr_val(root->root_path))) ==
      root) {
    w_ht_del(watched_roots, w_ht_ptr_val(root->root_path));
    removed = true;
  }
  pthread_mutex_unlock(&watch_list_lock);
  return removed;
}

// Given a filename, walk the current set of watches.
// If a watch is a prefix match for filename then we consider it to
// be an enclosing watch and we'll return the root path and the relative
// path to filename.
// Returns NULL if there were no matches.
// If multiple watches have the same prefix, it is undefined which one will
// match.
char *w_find_enclosing_root(const char *filename, char **relpath) {
  w_ht_iter_t i;
  w_root_t *root = NULL;
  w_string_t *name = w_string_new_typed(filename, W_STRING_BYTE);
  char *prefix = NULL;

  pthread_mutex_lock(&watch_list_lock);
  if (w_ht_first(watched_roots, &i)) do {
    auto root_name = (w_string_t *)w_ht_val_ptr(i.key);
    if (w_string_startswith(name, root_name) && (
          name->len == root_name->len /* exact match */ ||
          is_slash(name->buf[root_name->len]) /* dir container matches */)) {
      root = (w_root_t*)w_ht_val_ptr(i.value);
      w_root_addref(root);
      break;
    }
  } while (w_ht_next(watched_roots, &i));
  pthread_mutex_unlock(&watch_list_lock);

  if (!root) {
    goto out;
  }

  // extract the path portions
  prefix = (char*)malloc(root->root_path->len + 1);
  if (!prefix) {
    goto out;
  }
  memcpy(prefix, filename, root->root_path->len);
  prefix[root->root_path->len] = '\0';

  if (root->root_path->len == name->len) {
    *relpath = NULL;
  } else {
    *relpath = strdup(filename + root->root_path->len + 1);
  }

out:
  if (root) {
    w_root_delref_raw(root);
  }
  w_string_delref(name);

  return prefix;
}

json_t *w_root_stop_watch_all(void) {
  uint32_t roots_count, i;
  w_root_t **roots;
  w_ht_iter_t iter;
  json_t *stopped;

  pthread_mutex_lock(&watch_list_lock);
  roots_count = w_ht_size(watched_roots);
  roots = (w_root_t**)calloc(roots_count, sizeof(*roots));

  i = 0;
  if (w_ht_first(watched_roots, &iter)) do {
    auto root = (w_root_t *)w_ht_val_ptr(iter.value);
    w_root_addref(root);
    roots[i++] = root;
  } while (w_ht_next(watched_roots, &iter));

  stopped = json_array();
  for (i = 0; i < roots_count; i++) {
    w_root_t *root = roots[i];
    w_string_t *path = root->root_path;
    if (w_ht_del(watched_roots, w_ht_ptr_val(path))) {
      w_root_cancel(root);
      json_array_append_new(stopped, w_string_to_json(path));
    }
    w_root_delref_raw(root);
  }
  free(roots);
  pthread_mutex_unlock(&watch_list_lock);

  w_state_save();

  return stopped;
}

json_t *w_root_watch_list_to_json(void) {
  w_ht_iter_t iter;
  json_t *arr;

  arr = json_array();

  pthread_mutex_lock(&watch_list_lock);
  if (w_ht_first(watched_roots, &iter)) do {
    auto root = (w_root_t *)w_ht_val_ptr(iter.value);
    json_array_append_new(arr, w_string_to_json(root->root_path));
  } while (w_ht_next(watched_roots, &iter));
  pthread_mutex_unlock(&watch_list_lock);

  return arr;
}

bool w_root_save_state(json_t *state) {
  w_ht_iter_t root_iter;
  bool result = true;
  json_t *watched_dirs;

  watched_dirs = json_array();

  w_log(W_LOG_DBG, "saving state\n");

  pthread_mutex_lock(&watch_list_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    json_t *obj;
    json_t *triggers;
    struct read_locked_watchman_root lock;
    struct unlocked_watchman_root unlocked = {
        (w_root_t*)w_ht_val_ptr(root_iter.value)};

    obj = json_object();

    json_object_set_new(obj, "path",
                        w_string_to_json(unlocked.root->root_path));

    w_root_read_lock(&unlocked, "w_root_save_state", &lock);
    triggers = w_root_trigger_list_to_json(&lock);
    w_root_read_unlock(&lock, &unlocked);
    json_object_set_new(obj, "triggers", triggers);

    json_array_append_new(watched_dirs, obj);

  } while (w_ht_next(watched_roots, &root_iter));

  pthread_mutex_unlock(&watch_list_lock);

  json_object_set_new(state, "watched", watched_dirs);

  return result;
}

json_t *w_root_trigger_list_to_json(struct read_locked_watchman_root *lock) {
  w_ht_iter_t iter;
  json_t *arr;

  arr = json_array();
  if (w_ht_first(lock->root->commands, &iter)) do {
    auto cmd = (watchman_trigger_command *)w_ht_val_ptr(iter.value);

    json_array_append(arr, cmd->definition);
  } while (w_ht_next(lock->root->commands, &iter));

  return arr;
}

bool w_root_load_state(json_t *state) {
  json_t *watched;
  size_t i;

  watched = json_object_get(state, "watched");
  if (!watched) {
    return true;
  }

  if (!json_is_array(watched)) {
    return false;
  }

  for (i = 0; i < json_array_size(watched); i++) {
    json_t *obj = json_array_get(watched, i);
    bool created = false;
    const char *filename;
    json_t *triggers;
    size_t j;
    char *errmsg = NULL;
    struct write_locked_watchman_root lock;
    struct unlocked_watchman_root unlocked;

    triggers = json_object_get(obj, "triggers");
    filename = json_string_value(json_object_get(obj, "path"));
    if (!root_resolve(filename, true, &created, &errmsg, &unlocked)) {
      free(errmsg);
      continue;
    }

    w_root_lock(&unlocked, "w_root_load_state", &lock);

    /* re-create the trigger configuration */
    for (j = 0; j < json_array_size(triggers); j++) {
      json_t *tobj = json_array_get(triggers, j);
      json_t *rarray;
      struct watchman_trigger_command *cmd;

      // Legacy rules format
      rarray = json_object_get(tobj, "rules");
      if (rarray) {
        continue;
      }

      cmd = w_build_trigger_from_def(lock.root, tobj, &errmsg);
      if (!cmd) {
        w_log(W_LOG_ERR, "loading trigger for %s: %s\n",
              lock.root->root_path->buf, errmsg);
        free(errmsg);
        continue;
      }

      w_ht_replace(lock.root->commands, w_ht_ptr_val(cmd->triggername),
                   w_ht_ptr_val(cmd));
    }
    w_root_unlock(&lock, &unlocked);

    if (created) {
      if (!root_start(unlocked.root, &errmsg)) {
        w_log(W_LOG_ERR, "root_start(%s) failed: %s\n",
            unlocked.root->root_path->buf, errmsg);
        free(errmsg);
        w_root_cancel(unlocked.root);
      }
    }

    w_root_delref(&unlocked);
  }

  return true;
}

void w_root_free_watched_roots(void) {
  w_ht_iter_t root_iter;
  int last, interval;
  time_t started;

  // Reap any children so that we can release their
  // references on the root
  w_reap_children(true);

  pthread_mutex_lock(&watch_list_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    auto root = (w_root_t *)w_ht_val_ptr(root_iter.value);
    if (!w_root_cancel(root)) {
      signal_root_threads(root);
    }
  } while (w_ht_next(watched_roots, &root_iter));
  pthread_mutex_unlock(&watch_list_lock);

  last = live_roots;
  time(&started);
  w_log(W_LOG_DBG, "waiting for roots to cancel and go away %d\n", last);
  interval = 100;
  for (;;) {
    int current = __sync_fetch_and_add(&live_roots, 0);
    if (current == 0) {
      break;
    }
    if (time(NULL) > started + 3) {
      w_log(W_LOG_ERR, "%d roots were still live at exit\n", current);
      break;
    }
    if (current != last) {
      w_log(W_LOG_DBG, "waiting: %d live\n", current);
      last = current;
    }
    usleep(interval);
    interval = MIN(interval * 2, 1000000);
  }

  w_log(W_LOG_DBG, "all roots are gone\n");
}

/* vim:ts=2:sw=2:et:
 */
