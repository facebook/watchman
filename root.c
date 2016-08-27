/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

static w_ht_t *watched_roots = NULL;
volatile long live_roots = 0;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

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

static w_ht_val_t root_copy_val(w_ht_val_t val)
{
  w_root_t *root = w_ht_val_ptr(val);

  w_root_addref(root);

  return val;
}

static void root_del_val(w_ht_val_t val)
{
  w_root_t *root = w_ht_val_ptr(val);

  w_root_delref(root);
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
  pthread_mutex_lock(&root_lock);
  // it's possible that the root has already been removed and replaced with
  // another, so make sure we're removing the right object
  if (w_ht_val_ptr(w_ht_get(watched_roots, w_ht_ptr_val(root->root_path))) ==
      root) {
    w_ht_del(watched_roots, w_ht_ptr_val(root->root_path));
    removed = true;
  }
  pthread_mutex_unlock(&root_lock);
  return removed;
}

/* Returns true if the global config root_restrict_files is not defined or if
 * one of the files in root_restrict_files exists, false otherwise. */
static bool root_check_restrict(const char *watch_path)
{
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

static bool check_allowed_fs(const char *filename, char **errmsg)
{
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

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &i)) do {
    w_string_t *root_name = w_ht_val_ptr(i.key);
    if (w_string_startswith(name, root_name) && (
          name->len == root_name->len /* exact match */ ||
          is_slash(name->buf[root_name->len]) /* dir container matches */)) {
      root = w_ht_val_ptr(i.value);
      w_root_addref(root);
      break;
    }
  } while (w_ht_next(watched_roots, &i));
  pthread_mutex_unlock(&root_lock);

  if (!root) {
    goto out;
  }

  // extract the path portions
  prefix = malloc(root->root_path->len + 1);
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
    w_root_delref(root);
  }
  w_string_delref(name);

  return prefix;
}

static bool root_resolve(const char *filename, bool auto_watch, bool *created,
                         char **errmsg,
                         struct unlocked_watchman_root *unlocked) {
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
  pthread_mutex_lock(&root_lock);
  // This will addref if it returns root
  if (w_ht_lookup(watched_roots, w_ht_ptr_val(root_str), &root_val, true)) {
    root = w_ht_val_ptr(root_val);
  }
  pthread_mutex_unlock(&root_lock);
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
    time(&unlocked->root->last_cmd_timestamp);
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

  pthread_mutex_lock(&root_lock);
  existing = w_ht_val_ptr(w_ht_get(watched_roots,
                w_ht_ptr_val(root->root_path)));
  if (existing) {
    // Someone beat us in this race
    w_root_addref(existing);
    w_root_delref(root);
    root = existing;
    *created = false;
  } else {
    // adds 1 ref
    w_ht_set(watched_roots, w_ht_ptr_val(root->root_path), w_ht_ptr_val(root));
    *created = true;
  }
  pthread_mutex_unlock(&root_lock);

  // caller owns 1 ref
  unlocked->root = root;
  return true;
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

static bool root_start(w_root_t *root, char **errmsg)
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

static void signal_root_threads(w_root_t *root)
{
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

json_t *w_root_stop_watch_all(void)
{
  uint32_t roots_count, i;
  w_root_t **roots;
  w_ht_iter_t iter;
  json_t *stopped;

  pthread_mutex_lock(&root_lock);
  roots_count = w_ht_size(watched_roots);
  roots = calloc(roots_count, sizeof(*roots));

  i = 0;
  if (w_ht_first(watched_roots, &iter)) do {
    w_root_t *root = w_ht_val_ptr(iter.value);
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
    w_root_delref(root);
  }
  free(roots);
  pthread_mutex_unlock(&root_lock);

  w_state_save();

  return stopped;
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
      w_root_delref(unlocked->root);
      return false;
    }
    w_state_save();
  }
  return true;
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

json_t *w_root_watch_list_to_json(void)
{
  w_ht_iter_t iter;
  json_t *arr;

  arr = json_array();

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &iter)) do {
    w_root_t *root = w_ht_val_ptr(iter.value);
    json_array_append_new(arr, w_string_to_json(root->root_path));
  } while (w_ht_next(watched_roots, &iter));
  pthread_mutex_unlock(&root_lock);

  return arr;
}

bool w_root_load_state(json_t *state)
{
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

    w_root_delref(unlocked.root);
  }

  return true;
}

bool w_root_save_state(json_t *state)
{
  w_ht_iter_t root_iter;
  bool result = true;
  json_t *watched_dirs;

  watched_dirs = json_array();

  w_log(W_LOG_DBG, "saving state\n");

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    json_t *obj;
    json_t *triggers;
    struct read_locked_watchman_root lock;
    struct unlocked_watchman_root unlocked = {w_ht_val_ptr(root_iter.value)};

    obj = json_object();

    json_object_set_new(obj, "path",
                        w_string_to_json(unlocked.root->root_path));

    w_root_read_lock(&unlocked, "w_root_save_state", &lock);
    triggers = w_root_trigger_list_to_json(&lock);
    w_root_read_unlock(&lock, &unlocked);
    json_object_set_new(obj, "triggers", triggers);

    json_array_append_new(watched_dirs, obj);

  } while (w_ht_next(watched_roots, &root_iter));

  pthread_mutex_unlock(&root_lock);

  json_object_set_new(state, "watched", watched_dirs);

  return result;
}

void w_root_free_watched_roots(void)
{
  w_ht_iter_t root_iter;
  int last, interval;
  time_t started;

  // Reap any children so that we can release their
  // references on the root
  w_reap_children(true);

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    w_root_t *root = w_ht_val_ptr(root_iter.value);
    if (!w_root_cancel(root)) {
      signal_root_threads(root);
    }
  } while (w_ht_next(watched_roots, &root_iter));
  pthread_mutex_unlock(&root_lock);

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
