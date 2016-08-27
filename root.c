/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Returns true if we observe the change within the requested
 * time, false otherwise.
 * Must be called with the root UNLOCKED.  This function
 * will acquire and release the root lock.
 */
bool w_root_sync_to_now(struct unlocked_watchman_root *unlocked,
                        int timeoutms) {
  uint32_t tick;
  struct watchman_query_cookie cookie;
  w_string_t *path_str;
  w_stm_t file;
  int errcode = 0;
  struct timespec deadline;
  w_perf_t sample;
  struct write_locked_watchman_root lock;

  w_perf_start(&sample, "sync_to_now");

  if (pthread_cond_init(&cookie.cond, NULL)) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: cond_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }

  if (pthread_mutex_init(&cookie.lock, NULL)) {
    errcode = errno;
    pthread_cond_destroy(&cookie.cond);
    w_log(W_LOG_ERR, "sync_to_now: mutex_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }
  cookie.seen = false;
  pthread_mutex_lock(&cookie.lock);

  /* generate a cookie name: cookie prefix + id */
  w_root_lock(unlocked, "w_root_sync_to_now", &lock);
  tick = lock.root->ticks++;
  path_str = w_string_make_printf("%.*s%" PRIu32 "-%" PRIu32,
                                  lock.root->query_cookie_prefix->len,
                                  lock.root->query_cookie_prefix->buf,
                                  lock.root->number, tick);
  /* insert our cookie in the map */
  w_ht_set(lock.root->query_cookies, w_ht_ptr_val(path_str),
      w_ht_ptr_val(&cookie));
  w_root_unlock(&lock, unlocked);

  /* touch the file */
  file = w_stm_open(path_str->buf, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0700);
  if (!file) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: creat(%s) failed: %s\n",
        path_str->buf, strerror(errcode));
    goto out;
  }
  w_stm_close(file);

  /* compute deadline */
  w_timeoutms_to_abs_timespec(timeoutms, &deadline);

  w_log(W_LOG_DBG, "sync_to_now [%s] waiting\n", path_str->buf);

  /* timed cond wait (unlocks root lock, reacquires) */
  while (!cookie.seen) {
    errcode = pthread_cond_timedwait(&cookie.cond, &cookie.lock, &deadline);
    if (errcode && !cookie.seen) {
      w_log(W_LOG_ERR,
          "sync_to_now: %s timedwait failed: %d: istimeout=%d %s\n",
          path_str->buf, errcode, errcode == ETIMEDOUT, strerror(errcode));
      goto out;
    }
  }
  w_log(W_LOG_DBG, "sync_to_now [%s] done\n", path_str->buf);

out:
  pthread_mutex_unlock(&cookie.lock);
  w_root_lock(unlocked, "w_root_sync_to_now_done", &lock);

  // can't unlink the file until after the cookie has been observed because
  // we don't know which file got changed until we look in the cookie dir
  unlink(path_str->buf);
  w_ht_del(lock.root->query_cookies, w_ht_ptr_val(path_str));
  w_root_unlock(&lock, unlocked);

  // We want to know about all timeouts
  if (!cookie.seen) {
    w_perf_force_log(&sample);
  }

  if (w_perf_finish(&sample)) {
    w_perf_add_root_meta(&sample, unlocked->root);
    w_perf_add_meta(&sample, "sync_to_now",
                    json_pack("{s:b, s:i, s:i}",      //
                              "success", cookie.seen, //
                              "timeoutms", timeoutms, //
                              "errcode", errcode      //
                              ));
    w_perf_log(&sample);
  }

  w_string_delref(path_str);
  pthread_cond_destroy(&cookie.cond);
  pthread_mutex_destroy(&cookie.lock);

  w_perf_destroy(&sample);

  if (!cookie.seen) {
    errno = errcode;
    return false;
  }

  return true;
}

void stop_watching_dir(struct write_locked_watchman_root *lock,
                       struct watchman_dir *dir) {
  w_ht_iter_t i;
  w_string_t *dir_path = w_dir_copy_full_path(dir);

  w_log(W_LOG_DBG, "stop_watching_dir %.*s\n", dir_path->len, dir_path->buf);
  w_string_delref(dir_path);

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    stop_watching_dir(lock, child);
  } while (w_ht_next(dir->dirs, &i));

  lock->root->watcher_ops->root_stop_watch_dir(lock, dir);
}

bool did_file_change(struct watchman_stat *saved, struct watchman_stat *fresh) {
  /* we have to compare this way because the stat structure
   * may contain fields that vary and that don't impact our
   * understanding of the file */

#define FIELD_CHG(name) \
  if (saved->name != fresh->name) { \
    return true; \
  }

  // Can't compare with memcmp due to padding and garbage in the struct
  // on OpenBSD, which has a 32-bit tv_sec + 64-bit tv_nsec
#define TIMESPEC_FIELD_CHG(wat) { \
  struct timespec a = saved->wat##time; \
  struct timespec b = fresh->wat##time; \
  if (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec) { \
    return true; \
  } \
}

  FIELD_CHG(mode);

  if (!S_ISDIR(saved->mode)) {
    FIELD_CHG(size);
    FIELD_CHG(nlink);
  }
  FIELD_CHG(dev);
  FIELD_CHG(ino);
  FIELD_CHG(uid);
  FIELD_CHG(gid);
  // Don't care about st_blocks
  // Don't care about st_blksize
  // Don't care about st_atimespec
  TIMESPEC_FIELD_CHG(m);
  TIMESPEC_FIELD_CHG(c);

  return false;
}

// POSIX says open with O_NOFOLLOW should set errno to ELOOP if the path is a
// symlink. However, FreeBSD (which ironically originated O_NOFOLLOW) sets it to
// EMLINK.
#ifdef __FreeBSD__
#define ENOFOLLOWSYMLINK EMLINK
#else
#define ENOFOLLOWSYMLINK ELOOP
#endif

void struct_stat_to_watchman_stat(const struct stat *st,
                                  struct watchman_stat *target) {
  target->size = (off_t)st->st_size;
  target->mode = st->st_mode;
  target->uid = st->st_uid;
  target->gid = st->st_gid;
  target->ino = st->st_ino;
  target->dev = st->st_dev;
  target->nlink = st->st_nlink;
  memcpy(&target->atime, &st->WATCHMAN_ST_TIMESPEC(a),
      sizeof(target->atime));
  memcpy(&target->mtime, &st->WATCHMAN_ST_TIMESPEC(m),
      sizeof(target->mtime));
  memcpy(&target->ctime, &st->WATCHMAN_ST_TIMESPEC(c),
      sizeof(target->ctime));
}

void w_root_process_path(struct write_locked_watchman_root *lock,
    struct watchman_pending_collection *coll, w_string_t *full_path,
    struct timeval now, int flags,
    struct watchman_dir_ent *pre_stat)
{
  /* From a particular query's point of view, there are four sorts of cookies we
   * can observe:
   * 1. Cookies that this query has created. This marks the end of this query's
   *    sync_to_now, so we hide it from the results.
   * 2. Cookies that another query on the same watch by the same process has
   *    created. This marks the end of that other query's sync_to_now, so from
   *    the point of view of this query we turn a blind eye to it.
   * 3. Cookies created by another process on the same watch. We're independent
   *    of other processes, so we report these.
   * 4. Cookies created by a nested watch by the same or a different process.
   *    We're independent of other watches, so we report these.
   *
   * The below condition is true for cases 1 and 2 and false for 3 and 4.
   */
  if (w_string_startswith(full_path, lock->root->query_cookie_prefix)) {
    struct watchman_query_cookie *cookie;
    bool consider_cookie =
        (lock->root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS)
            ? ((flags & W_PENDING_VIA_NOTIFY) || !lock->root->done_initial)
            : true;

    if (!consider_cookie) {
      // Never allow cookie files to show up in the tree
      return;
    }

    cookie = w_ht_val_ptr(
        w_ht_get(lock->root->query_cookies, w_ht_ptr_val(full_path)));
    w_log(W_LOG_DBG, "cookie! %.*s cookie=%p\n",
        full_path->len, full_path->buf, cookie);

    if (cookie) {
      cookie->seen = true;
      pthread_cond_signal(&cookie->cond);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(full_path, lock->root->root_path)
      || (flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(lock, coll, full_path, now,
        (flags & W_PENDING_RECURSIVE) == W_PENDING_RECURSIVE);
  } else {
    stat_path(lock, coll, full_path, now, flags, pre_stat);
  }
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(struct write_locked_watchman_root *lock,
                         struct watchman_dir *dir, struct timeval now,
                         bool recursive) {
  w_ht_iter_t i;

  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = w_ht_val_ptr(i.value);

    if (file->exists) {
      w_string_t *full_name = w_dir_path_cat_str(dir, w_file_get_name(file));
      w_log(W_LOG_DBG, "mark_deleted: %.*s\n", full_name->len, full_name->buf);
      w_string_delref(full_name);
      file->exists = false;
      w_root_mark_file_changed(lock, file, now);
    }

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    w_root_mark_deleted(lock, child, now, true);
  } while (w_ht_next(dir->dirs, &i));
}

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

void w_root_teardown(w_root_t *root)
{
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

static bool start_detached_root_thread(w_root_t *root, char **errmsg,
    void*(*func)(void*), pthread_t *thr) {
  pthread_attr_t attr;
  int err;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  w_root_addref(root);
  err = pthread_create(thr, &attr, func, root);
  pthread_attr_destroy(&attr);

  if (err == 0) {
    return true;
  }

  ignore_result(asprintf(errmsg,
        "failed to pthread_create: %s\n", strerror(err)));
  w_root_delref(root);
  return false;
}

bool root_start(w_root_t *root, char **errmsg)
{
  if (!start_detached_root_thread(root, errmsg,
        run_notify_thread, &root->notify_thread)) {
    return false;
  }

  // Wait for it to signal that the watcher has been initialized
  w_pending_coll_lock_and_wait(&root->pending, -1 /* infinite */);
  w_pending_coll_unlock(&root->pending);

  if (!start_detached_root_thread(root, errmsg,
        run_io_thread, &root->io_thread)) {
    w_root_cancel(root);
    return false;
  }
  return true;
}

void signal_root_threads(w_root_t *root) {
  // Send SIGUSR1 to interrupt blocking syscalls on the
  // worker threads.  They'll self-terminate.
  if (!pthread_equal(root->notify_thread, pthread_self())) {
    pthread_kill(root->notify_thread, SIGUSR1);
  }
  w_pending_coll_ping(&root->pending);
  root->watcher_ops->root_signal_threads(root);
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

// Cancels a watch.
bool w_root_cancel(w_root_t *root /* don't care about locked state */)
{
  bool cancelled = false;

  if (!root->cancelled) {
    cancelled = true;

    w_log(W_LOG_DBG, "marked %s cancelled\n",
        root->root_path->buf);
    root->cancelled = true;

    signal_root_threads(root);
  }

  return cancelled;
}

bool w_root_stop_watch(struct unlocked_watchman_root *unlocked)
{
  bool stopped = remove_root_from_watched(unlocked->root);

  if (stopped) {
    w_root_cancel(unlocked->root);
    w_state_save(); // this is what required that we are not locked
  }
  signal_root_threads(unlocked->root);

  return stopped;
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
