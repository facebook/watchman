/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// POSIX says open with O_NOFOLLOW should set errno to ELOOP if the path is a
// symlink. However, FreeBSD (which ironically originated O_NOFOLLOW) sets it to
// EMLINK.
#ifdef __FreeBSD__
#define ENOFOLLOWSYMLINK EMLINK
#else
#define ENOFOLLOWSYMLINK ELOOP
#endif

void handle_open_errno(struct write_locked_watchman_root *lock,
                       struct watchman_dir *dir, struct timeval now,
                       const char *syscall, int err, const char *reason) {
  w_string_t *dir_name = w_dir_copy_full_path(dir);
  w_string_t *warn = NULL;
  bool log_warning = true;
  bool transient = false;

  if (err == ENOENT || err == ENOTDIR || err == ENOFOLLOWSYMLINK) {
    log_warning = false;
    transient = false;
  } else if (err == EACCES || err == EPERM) {
    log_warning = true;
    transient = false;
  } else if (err == ENFILE || err == EMFILE) {
    set_poison_state(dir_name, now, syscall, err, strerror(err));
    w_string_delref(dir_name);
    return;
  } else {
    log_warning = true;
    transient = true;
  }

  if (w_string_equal(dir_name, lock->root->root_path)) {
    if (!transient) {
      w_log(W_LOG_ERR,
            "%s(%.*s) -> %s. Root was deleted; cancelling watch\n",
            syscall, dir_name->len, dir_name->buf,
            reason ? reason : strerror(err));
      w_root_cancel(lock->root);
      w_string_delref(dir_name);
      return;
    }
  }

  warn = w_string_make_printf(
      "%s(%.*s) -> %s. Marking this portion of the tree deleted",
      syscall, dir_name->len, dir_name->buf,
      reason ? reason : strerror(err));

  w_log(err == ENOENT ? W_LOG_DBG : W_LOG_ERR, "%.*s\n", warn->len, warn->buf);
  if (log_warning) {
    w_root_set_warning(lock, warn);
  }
  w_string_delref(warn);

  stop_watching_dir(lock, dir);
  w_root_mark_deleted(lock, dir, now, true);
  w_string_delref(dir_name);
}

void w_root_set_warning(struct write_locked_watchman_root *lock,
                        w_string_t *str) {
  if (lock->root->warning) {
    w_string_delref(lock->root->warning);
  }
  lock->root->warning = str;
  if (lock->root->warning) {
    w_string_addref(lock->root->warning);
  }
}

void free_file_node(w_root_t *root, struct watchman_file *file)
{
  root->watcher_ops->file_free(file);
  if (file->symlink_target) {
    w_string_delref(file->symlink_target);
  }
  free(file);
}

static bool root_has_subscriptions(w_root_t *root) {
  bool has_subscribers = false;
  w_ht_iter_t iter;

  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_user_client *client = w_ht_val_ptr(iter.value);
    w_ht_iter_t citer;

    if (w_ht_first(client->subscriptions, &citer)) do {
      struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);

      if (sub->root == root) {
        has_subscribers = true;
        break;
      }

    } while (w_ht_next(client->subscriptions, &citer));
  } while (!has_subscribers && w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);
  return has_subscribers;
}

// This is a little tricky.  We have to be called with root->lock
// held, but we must not call w_root_stop_watch with the lock held,
// so we return true if the caller should do that
bool consider_reap(struct write_locked_watchman_root *lock) {
  time_t now;
  w_root_t *root = lock->root;

  if (root->idle_reap_age == 0) {
    return false;
  }

  time(&now);

  if (now > root->last_cmd_timestamp + root->idle_reap_age &&
      (root->commands == NULL || w_ht_size(root->commands) == 0) &&
      (now > root->last_reap_timestamp) &&
      !root_has_subscriptions(root)) {
    // We haven't had any activity in a while, and there are no registered
    // triggers or subscriptions against this watch.
    w_log(W_LOG_ERR, "root %.*s has had no activity in %d seconds and has "
        "no triggers or subscriptions, cancelling watch.  "
        "Set idle_reap_age_seconds in your .watchmanconfig to control "
        "this behavior\n",
        root->root_path->len, root->root_path->buf, root->idle_reap_age);
    return true;
  }

  root->last_reap_timestamp = now;

  return false;
}

void w_root_addref(w_root_t *root)
{
  w_refcnt_add(&root->refcnt);
}

void w_root_schedule_recrawl(w_root_t *root, const char *why)
{
  if (!root->should_recrawl) {
    if (root->last_recrawl_reason) {
      w_string_delref(root->last_recrawl_reason);
    }

    root->last_recrawl_reason = w_string_make_printf(
        "%.*s: %s",
        root->root_path->len, root->root_path->buf, why);

    w_log(W_LOG_ERR, "%.*s: %s: scheduling a tree recrawl\n",
        root->root_path->len, root->root_path->buf, why);
  }
  root->should_recrawl = true;
  signal_root_threads(root);
}

// Caller must have locked root
json_t *w_root_trigger_list_to_json(struct read_locked_watchman_root *lock)
{
  w_ht_iter_t iter;
  json_t *arr;

  arr = json_array();
  if (w_ht_first(lock->root->commands, &iter)) do {
    struct watchman_trigger_command *cmd = w_ht_val_ptr(iter.value);

    json_array_append(arr, cmd->definition);
  } while (w_ht_next(lock->root->commands, &iter));

  return arr;
}

/* vim:ts=2:sw=2:et:
 */
