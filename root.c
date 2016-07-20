/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

static w_ht_t *watched_roots = NULL;
static volatile long live_roots = 0;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static long next_root_number = 1;

static void crawler(w_root_t *root, struct watchman_pending_collection *coll,
    w_string_t *dir_name, struct timeval now, bool recursive);

static void w_root_teardown(w_root_t *root);

static void delete_trigger(w_ht_val_t val)
{
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

static void delete_dir(w_ht_val_t val)
{
  struct watchman_dir *dir = w_ht_val_ptr(val);

  w_log(W_LOG_DBG, "delete_dir(%.*s)\n", dir->path->len, dir->path->buf);

  w_string_delref(dir->path);
  dir->path = NULL;

  if (dir->files) {
    w_ht_free(dir->files);
    dir->files = NULL;
  }
  if (dir->dirs) {
    w_ht_free(dir->dirs);
    dir->dirs = NULL;
  }
  free(dir);
}

static const struct watchman_hash_funcs dirname_hash_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  delete_dir
};

static void load_root_config(w_root_t *root, const char *path)
{
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

static size_t root_init_offset = offsetof(w_root_t, _init_sentinel_);

// internal initialization for root
static bool w_root_init(w_root_t *root, char **errmsg)
{
  struct watchman_dir *dir;
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

  root->dirname_to_dir =
      w_ht_new((uint32_t)cfg_get_int(root, CFG_HINT_NUM_DIRS, HINT_NUM_DIRS),
               &dirname_hash_funcs);
  root->ticks = 1;

  // "manually" populate the initial dir, as the dir resolver will
  // try to find its parent and we don't want it to for the root
  dir = calloc(1, sizeof(*dir));
  dir->path = root->root_path;
  w_string_addref(dir->path);
  w_ht_set(root->dirname_to_dir, w_ht_ptr_val(dir->path), w_ht_ptr_val(dir));

  time(&root->last_cmd_timestamp);

  return root;
}

static json_t *config_get_ignore_vcs(w_root_t *root)
{
  json_t *ignores = cfg_get_json(root, "ignore_vcs");
  if (ignores && !json_is_array(ignores)) {
    return NULL;
  }

  if (ignores) {
    // incref so that the caller can simply decref whatever we return
    json_incref(ignores);
  } else {
    // default to a well-known set of vcs's
    ignores = json_pack("[sss]", ".git", ".svn", ".hg");
  }
  return ignores;
}

static bool apply_ignore_vcs_configuration(w_root_t *root, char **errmsg)
{
  w_string_t *name;
  w_string_t *fullname;
  uint8_t i;
  json_t *ignores;
  char hostname[256];
  struct stat st;

  ignores = config_get_ignore_vcs(root);
  if (!ignores) {
    ignore_result(asprintf(errmsg, "ignore_vcs must be an array of strings"));
    return false;
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    const json_t *jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      ignore_result(asprintf(errmsg,
          "ignore_vcs must be an array of strings"));
      json_decref(ignores);
      return false;
    }

    name = json_to_w_string(jignore);
    fullname = w_string_path_cat(root->root_path, name);

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (w_ht_get(root->ignore.ignore_dirs, w_ht_ptr_val(fullname))) {
      w_string_delref(fullname);
      continue;
    }

    w_ignore_addstr(&root->ignore, fullname, true);

    // While we're at it, see if we can find out where to put our
    // query cookie information
    if (root->query_cookie_dir == NULL &&
        w_lstat(fullname->buf, &st, root->case_sensitive) == 0 &&
        S_ISDIR(st.st_mode)) {
      // root/{.hg,.git,.svn}
      root->query_cookie_dir = w_string_path_cat(root->root_path, name);
    }
    w_string_delref(fullname);
  }

  json_decref(ignores);

  if (root->query_cookie_dir == NULL) {
    w_string_addref(root->root_path);
    root->query_cookie_dir = root->root_path;
  }
  gethostname(hostname, sizeof(hostname));
  hostname[sizeof(hostname) - 1] = '\0';

  root->query_cookie_prefix = w_string_make_printf(
      "%.*s%c" WATCHMAN_COOKIE_PREFIX "%s-%d-", root->query_cookie_dir->len,
      root->query_cookie_dir->buf, WATCHMAN_DIR_SEP, hostname, (int)getpid());
  return true;
}

static void apply_ignore_configuration(w_root_t *root)
{
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

static w_root_t *w_root_new(const char *path, char **errmsg)
{
  w_root_t *root = calloc(1, sizeof(*root));
  pthread_mutexattr_t attr;

  assert(root != NULL);

  root->refcnt = 1;
  w_refcnt_add(&live_roots);
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&root->lock, &attr);
  pthread_mutexattr_destroy(&attr);

  root->case_sensitive = is_case_sensitive_filesystem(path);

  w_pending_coll_init(&root->pending);
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
    w_root_delref(root);
    return NULL;
  }

  if (!w_root_init(root, errmsg)) {
    w_root_delref(root);
    return NULL;
  }
  return root;
}

void w_root_lock(w_root_t *root, const char *purpose)
{
  int err;

  err = pthread_mutex_lock(&root->lock);
  if (err != 0) {
    w_log(W_LOG_FATAL, "lock (%s) [%.*s]: %s\n",
        purpose,
        root->root_path->len,
        root->root_path->buf,
        strerror(err)
    );
  }
  root->lock_reason = purpose;
}

bool w_root_lock_with_timeout(w_root_t *root, const char *purpose,
                              int timeoutms) {
  struct timespec ts;
  struct timeval delta, now, target;
  int err;

  if (timeoutms <= 0) {
    // Special case an immediate check, because the implementation of
    // pthread_mutex_timedlock may return immediately if we are already
    // past-due.
    err = pthread_mutex_trylock(&root->lock);
  } else {
    // Add timeout to current time, convert to absolute timespec
    gettimeofday(&now, NULL);
    delta.tv_sec = timeoutms / 1000;
    delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
    w_timeval_add(now, delta, &target);
    w_timeval_to_timespec(target, &ts);

    err = pthread_mutex_timedlock(&root->lock, &ts);
  }
  if (err == ETIMEDOUT || err == EBUSY) {
    w_log(W_LOG_ERR,
          "lock (%s) [%.*s] failed after %dms, current lock purpose: %s\n",
          purpose, root->root_path->len, root->root_path->buf, timeoutms,
          root->lock_reason);
    errno = ETIMEDOUT;
    return false;
  }
  if (err != 0) {
    w_log(W_LOG_FATAL, "lock (%s) [%.*s]: %s\n",
        purpose,
        root->root_path->len,
        root->root_path->buf,
        strerror(err)
    );
  }
  root->lock_reason = purpose;
  return true;
}

void w_root_unlock(w_root_t *root)
{
  int err;

  root->lock_reason = NULL;
  err = pthread_mutex_unlock(&root->lock);
  if (err != 0) {
    w_log(W_LOG_FATAL, "lock: [%.*s] %s\n",
        root->root_path->len,
        root->root_path->buf,
        strerror(err)
    );
  }
}

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
bool w_root_sync_to_now(w_root_t *root, int timeoutms)
{
  uint32_t tick;
  struct watchman_query_cookie cookie;
  w_string_t *path_str;
  w_stm_t file;
  int errcode = 0;
  struct timespec deadline;
  w_perf_t sample;

  w_perf_start(&sample, "sync_to_now");

  if (pthread_cond_init(&cookie.cond, NULL)) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: cond_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }
  cookie.seen = false;

  /* generate a cookie name: cookie prefix + id */
  w_root_lock(root, "w_root_sync_to_now");
  tick = root->ticks++;
  path_str = w_string_make_printf("%.*s%" PRIu32 "-%" PRIu32,
                                  root->query_cookie_prefix->len,
                                  root->query_cookie_prefix->buf,
                                  root->number, tick);
  /* insert our cookie in the map */
  w_ht_set(root->query_cookies, w_ht_ptr_val(path_str),
      w_ht_ptr_val(&cookie));

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
    errcode = pthread_cond_timedwait(&cookie.cond, &root->lock, &deadline);
    if (errcode && !cookie.seen) {
      w_log(W_LOG_ERR,
          "sync_to_now: %s timedwait failed: %d: istimeout=%d %s\n",
          path_str->buf, errcode, errcode == ETIMEDOUT, strerror(errcode));
      goto out;
    }
  }
  w_log(W_LOG_DBG, "sync_to_now [%s] done\n", path_str->buf);

out:
  // can't unlink the file until after the cookie has been observed because
  // we don't know which file got changed until we look in the cookie dir
  unlink(path_str->buf);
  w_ht_del(root->query_cookies, w_ht_ptr_val(path_str));
  w_root_unlock(root);

  // We want to know about all timeouts
  if (!cookie.seen) {
    w_perf_force_log(&sample);
  }

  if (w_perf_finish(&sample)) {
    w_perf_add_root_meta(&sample, root);
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

  w_perf_destroy(&sample);

  if (!cookie.seen) {
    errno = errcode;
    return false;
  }

  return true;
}

bool w_root_process_pending(w_root_t *root,
    struct watchman_pending_collection *coll,
    bool pull_from_root)
{
  struct watchman_pending_fs *p, *pending;

  if (pull_from_root) {
    // You MUST own root->pending lock for this
    w_pending_coll_append(coll, &root->pending);
  }

  if (!coll->pending) {
    return false;
  }

  w_log(W_LOG_DBG, "processing %d events in %s\n",
      w_pending_coll_size(coll), root->root_path->buf);

  // Steal the contents
  pending = coll->pending;
  coll->pending = NULL;
  w_pending_coll_drain(coll);

  while (pending) {
    p = pending;
    pending = p->next;

    if (!root->cancelled) {
      w_root_process_path(root, coll, p->path, p->now, p->flags, NULL);
    }

    w_pending_fs_free(p);
  }

  return true;
}

struct watchman_dir *w_root_resolve_dir(w_root_t *root,
    w_string_t *dir_name, bool create)
{
  struct watchman_dir *dir, *parent;
  w_string_t *parent_name;

  dir = w_ht_val_ptr(w_ht_get(root->dirname_to_dir, w_ht_ptr_val(dir_name)));
  if (dir || !create) {
    return dir;
  }

  parent_name = w_string_dirname(dir_name);
  parent = w_root_resolve_dir(root, parent_name, create);
  w_string_delref(parent_name);

  assert(parent != NULL);

  dir = calloc(1, sizeof(*dir));
  dir->path = dir_name;
  dir->last_check_existed = true;
  w_string_addref(dir->path);

  if (!parent->dirs) {
    parent->dirs = w_ht_new(2, &w_ht_string_funcs);
  }

  assert(w_ht_set(parent->dirs, w_ht_ptr_val(dir_name), w_ht_ptr_val(dir)));
  assert(w_ht_set(root->dirname_to_dir,
        w_ht_ptr_val(dir_name), w_ht_ptr_val(dir)));

  return dir;
}

static void apply_dir_size_hint(struct watchman_dir *dir,
    uint32_t ndirs, uint32_t nfiles) {

  if (nfiles > 0) {
    if (!dir->files) {
      dir->files = w_ht_new(nfiles, &w_ht_string_funcs);
    }
  }
  if (!dir->dirs && ndirs > 0) {
    dir->dirs = w_ht_new(ndirs, &w_ht_string_funcs);
  }
}

static void watch_file(w_root_t *root, struct watchman_file *file)
{
  root->watcher_ops->root_start_watch_file(root, file);
}

static void stop_watching_file(w_root_t *root, struct watchman_file *file)
{
  root->watcher_ops->root_stop_watch_file(root, file);
}

static void remove_from_file_list(w_root_t *root, struct watchman_file *file)
{
  if (root->latest_file == file) {
    root->latest_file = file->next;
  }
  if (file->next) {
    file->next->prev = file->prev;
  }
  if (file->prev) {
    file->prev->next = file->next;
  }
}

static void remove_from_suffix_list(w_root_t *root, struct watchman_file *file)
{
  w_string_t *suffix = w_string_suffix(file->name);
  struct watchman_file *sufhead;

  if (!suffix) {
    return;
  }

  sufhead = w_ht_val_ptr(w_ht_get(root->suffixes, w_ht_ptr_val(suffix)));
  if (sufhead) {
    if (file->suffix_prev) {
      file->suffix_prev->suffix_next = file->suffix_next;
    }
    if (file->suffix_next) {
      file->suffix_next->suffix_prev = file->suffix_prev;
    }
    if (sufhead == file) {
      sufhead = file->suffix_next;
      w_ht_replace(root->suffixes, w_ht_ptr_val(suffix),
          w_ht_ptr_val(sufhead));
    }
  }

  w_string_delref(suffix);
}

void w_root_mark_file_changed(w_root_t *root, struct watchman_file *file,
    struct timeval now)
{
  if (file->exists) {
    watch_file(root, file);
  } else {
    stop_watching_file(root, file);
  }

  file->otime.tv = now;
  file->otime.ticks = root->ticks;

  if (root->latest_file != file) {
    // unlink from list
    remove_from_file_list(root, file);

    // and move to the head
    file->next = root->latest_file;
    if (file->next) {
      file->next->prev = file;
    }
    file->prev = NULL;
    root->latest_file = file;
  }

  // Flag that we have pending trigger info
  root->pending_trigger_tick = root->ticks;
  root->pending_sub_tick = root->ticks;
}

struct watchman_file *w_root_resolve_file(w_root_t *root,
    struct watchman_dir *dir, w_string_t *file_name,
    struct timeval now)
{
  struct watchman_file *file, *sufhead;
  w_string_t *suffix;

  if (dir->files) {
    file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
    if (file) {
      return file;
    }
  } else {
    dir->files = w_ht_new(2, &w_ht_string_funcs);
  }

  file = calloc(1, sizeof(*file));
  file->name = file_name;
  w_string_addref(file->name);
  file->parent = dir;
  file->exists = true;
  file->ctime.ticks = root->ticks;
  file->ctime.tv = now;

  suffix = w_string_suffix(file_name);
  if (suffix) {
    sufhead = w_ht_val_ptr(w_ht_get(root->suffixes, w_ht_ptr_val(suffix)));
    file->suffix_next = sufhead;
    if (sufhead) {
      sufhead->suffix_prev = file;
    }
    w_ht_replace(root->suffixes, w_ht_ptr_val(suffix), w_ht_ptr_val(file));
    w_string_delref(suffix);
  }

  w_ht_set(dir->files, w_ht_ptr_val(file->name), w_ht_ptr_val(file));
  watch_file(root, file);

  return file;
}

void stop_watching_dir(w_root_t *root, struct watchman_dir *dir)
{
  w_ht_iter_t i;

  w_log(W_LOG_DBG, "stop_watching_dir %.*s\n",
      dir->path->len, dir->path->buf);

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    stop_watching_dir(root, child);
  } while (w_ht_next(dir->dirs, &i));

  root->watcher_ops->root_stop_watch_dir(root, dir);
}

static bool did_file_change(struct watchman_stat *saved,
    struct watchman_stat *fresh)
{
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

static void struct_stat_to_watchman_stat(const struct stat *st,
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

static void stat_path(w_root_t *root,
    struct watchman_pending_collection *coll, w_string_t *full_path,
    struct timeval now,
    int flags,
    struct watchman_dir_ent *pre_stat)
{
  struct watchman_stat st;
  int res, err;
  char path[WATCHMAN_NAME_MAX];
  struct watchman_dir *dir;
  struct watchman_dir *dir_ent = NULL;
  struct watchman_file *file = NULL;
  w_string_t *dir_name;
  w_string_t *file_name;
  bool recursive = flags & W_PENDING_RECURSIVE;
  bool via_notify = flags & W_PENDING_VIA_NOTIFY;

  if (w_ht_get(root->ignore.ignore_dirs, w_ht_ptr_val(full_path))) {
    w_log(W_LOG_DBG, "%.*s matches ignore_dir rules\n",
        full_path->len, full_path->buf);
    return;
  }

  if (full_path->len > sizeof(path)-1) {
    w_log(W_LOG_FATAL, "path %.*s is too big\n",
        full_path->len, full_path->buf);
  }

  memcpy(path, full_path->buf, full_path->len);
  path[full_path->len] = 0;

  dir_name = w_string_dirname(full_path);
  file_name = w_string_basename(full_path);
  dir = w_root_resolve_dir(root, dir_name, true);

  if (dir->files) {
    file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
  }

  if (dir->dirs) {
    dir_ent = w_ht_val_ptr(w_ht_get(dir->dirs, w_ht_ptr_val(full_path)));
  }

  if (pre_stat && pre_stat->has_stat) {
    memcpy(&st, &pre_stat->stat, sizeof(st));
    res = 0;
    err = 0;
  } else {
    struct stat struct_stat;
    res = w_lstat(path, &struct_stat, root->case_sensitive);
    err = res == 0 ? 0 : errno;
    w_log(W_LOG_DBG, "w_lstat(%s) file=%p dir=%p res=%d %s\n",
        path, file, dir_ent, res, strerror(err));
    if (err == 0) {
      struct_stat_to_watchman_stat(&struct_stat, &st);
    } else {
      // To suppress warning on win32
      memset(&st, 0, sizeof(st));
    }
  }

  if (res && (err == ENOENT || err == ENOTDIR)) {
    /* it's not there, update our state */
    if (dir_ent) {
      w_root_mark_deleted(root, dir_ent, now, true);
      w_log(W_LOG_DBG, "w_lstat(%s) -> %s so stopping watch on %.*s\n", path,
            strerror(err), dir_ent->path->len, dir_ent->path->buf);
      stop_watching_dir(root, dir_ent);
    }
    if (file) {
      if (file->exists) {
        w_log(W_LOG_DBG, "w_lstat(%s) -> %s so marking %.*s deleted\n",
            path, strerror(err), file->name->len, file->name->buf);
        file->exists = false;
        w_root_mark_file_changed(root, file, now);
      }
    } else {
      // It was created and removed before we could ever observe it
      // in the filesystem.  We need to generate a deleted file
      // representation of it now, so that subscription clients can
      // be notified of this event
      file = w_root_resolve_file(root, dir, file_name, now);
      w_log(W_LOG_DBG, "w_lstat(%s) -> %s and file node was NULL. "
          "Generating a deleted node.\n", path, strerror(err));
      file->exists = false;
      w_root_mark_file_changed(root, file, now);
    }

    if (!root->case_sensitive && !w_string_equal(dir_name, root->root_path) &&
        dir->last_check_existed) {
      /* If we rejected the name because it wasn't canonical,
       * we need to ensure that we look in the parent dir to discover
       * the new item(s) */
      w_log(W_LOG_DBG, "we're case insensitive, and %s is ENOENT, "
                       "speculatively look at parent dir %.*s\n",
            path, dir_name->len, dir_name->buf);
      w_pending_coll_add(coll, dir_name, now, W_PENDING_CRAWL_ONLY);
    }

  } else if (res) {
    w_log(W_LOG_ERR, "w_lstat(%s) %d %s\n",
        path, err, strerror(err));
  } else {
    if (!file) {
      file = w_root_resolve_file(root, dir, file_name, now);
    }

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = root->ticks;
      file->ctime.tv = now;
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->stat, &st)) {
      w_log(W_LOG_DBG,
          "file changed exists=%d via_notify=%d stat-changed=%d isdir=%d %s\n",
          (int)file->exists,
          (int)via_notify,
          (int)(file->exists && !via_notify),
          S_ISDIR(st.mode),
          path
      );
      file->exists = true;
      w_root_mark_file_changed(root, file, now);
    }

    memcpy(&file->stat, &st, sizeof(file->stat));

#ifndef _WIN32
    if (file->symlink_target) {
      w_string_delref(file->symlink_target);
      file->symlink_target = NULL;
    }

    // check for symbolic link
    if (S_ISLNK(st.mode)) {
      char link_target_path[WATCHMAN_NAME_MAX];
      ssize_t tlen = 0;

      tlen = readlink(path, link_target_path, WATCHMAN_NAME_MAX);
      if (tlen < 0 || tlen >= WATCHMAN_NAME_MAX) {
        w_log(W_LOG_ERR,
            "readlink(%s) errno=%d tlen=%d\n", path, errno, (int)tlen);
      } else {
        file->symlink_target = w_string_new_len_typed(link_target_path, tlen,
            W_STRING_BYTE);
      }
    }
#endif

    if (S_ISDIR(st.mode)) {
      if (dir_ent == NULL) {
        recursive = true;
      } else {
        // Ensure that we believe that this node exists
        dir_ent->last_check_existed = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!w_ht_get(root->ignore.ignore_vcs, w_ht_ptr_val(dir_name)) ||
          // but do if we're looking at the cookie dir (stat_path is never
          // called for the root itself)
          w_string_equal(full_path, root->query_cookie_dir)) {

        if (!root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
          /* we always need to crawl, but may not need to be fully recursive */
          w_pending_coll_add(coll, full_path, now,
              W_PENDING_CRAWL_ONLY | (recursive ? W_PENDING_RECURSIVE : 0));
        } else {
          /* we get told about changes on the child, so we only
           * need to crawl if we've never seen the dir before.
           * An exception is that fsevents will only report the root
           * of a dir rename and not a rename event for all of its
           * children. */
          if (recursive) {
            w_pending_coll_add(coll, full_path, now,
                W_PENDING_RECURSIVE|W_PENDING_CRAWL_ONLY);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      w_root_mark_deleted(root, dir_ent, now, true);
    }
    if ((root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) &&
        !S_ISDIR(st.mode) && !w_string_equal(dir_name, root->root_path) &&
        dir->last_check_existed) {
      /* Make sure we update the mtime on the parent directory. */
      w_pending_coll_add(coll, dir_name, now, flags & W_PENDING_VIA_NOTIFY);
    }
  }

  // out is only used on some platforms, so on others compilers will complain
  // about it being unused
  goto out;

out:
  w_string_delref(dir_name);
  w_string_delref(file_name);
}


void w_root_process_path(w_root_t *root,
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
  if (w_string_startswith(full_path, root->query_cookie_prefix)) {
    struct watchman_query_cookie *cookie;
    bool consider_cookie =
      (root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) ?
      ((flags & W_PENDING_VIA_NOTIFY) || !root->done_initial) : true;

    if (!consider_cookie) {
      // Never allow cookie files to show up in the tree
      return;
    }

    cookie = w_ht_val_ptr(w_ht_get(root->query_cookies,
                                   w_ht_ptr_val(full_path)));
    w_log(W_LOG_DBG, "cookie! %.*s cookie=%p\n",
        full_path->len, full_path->buf, cookie);

    if (cookie) {
      cookie->seen = true;
      pthread_cond_signal(&cookie->cond);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(full_path, root->root_path)
      || (flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(root, coll, full_path, now,
        (flags & W_PENDING_RECURSIVE) == W_PENDING_RECURSIVE);
  } else {
    stat_path(root, coll, full_path, now, flags, pre_stat);
  }
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, bool recursive)
{
  w_ht_iter_t i;

  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = w_ht_val_ptr(i.value);

    if (file->exists) {
      w_log(W_LOG_DBG, "mark_deleted: %.*s%c%.*s\n",
          dir->path->len, dir->path->buf,
          WATCHMAN_DIR_SEP,
          file->name->len, file->name->buf);
      file->exists = false;
      w_root_mark_file_changed(root, file, now);
    }

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    w_root_mark_deleted(root, child, now, true);
  } while (w_ht_next(dir->dirs, &i));
}

void handle_open_errno(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, const char *syscall, int err, const char *reason)
{
  w_string_t *dir_name = dir->path;
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
    set_poison_state(root, dir->path, now, syscall, err,
        strerror(err));
    return;
  } else {
    log_warning = true;
    transient = true;
  }

  if (w_string_equal(dir_name, root->root_path)) {
    if (!transient) {
      w_log(W_LOG_ERR,
            "%s(%.*s) -> %s. Root was deleted; cancelling watch\n",
            syscall, dir_name->len, dir_name->buf,
            reason ? reason : strerror(err));
      w_root_cancel(root);
      return;
    }
  }

  warn = w_string_make_printf(
      "%s(%.*s) -> %s. Marking this portion of the tree deleted",
      syscall, dir_name->len, dir_name->buf,
      reason ? reason : strerror(err));

  w_log(err == ENOENT ? W_LOG_DBG : W_LOG_ERR, "%.*s\n", warn->len, warn->buf);
  if (log_warning) {
    w_root_set_warning(root, warn);
  }
  w_string_delref(warn);

  stop_watching_dir(root, dir);
  w_root_mark_deleted(root, dir, now, true);
}

void w_root_set_warning(w_root_t *root, w_string_t *str) {
  if (root->warning) {
    w_string_delref(root->warning);
  }
  root->warning = str;
  if (root->warning) {
    w_string_addref(root->warning);
  }
}

void set_poison_state(w_root_t *root, w_string_t *dir,
    struct timeval now, const char *syscall, int err, const char *reason)
{
  char *why = NULL;

  unused_parameter(root);

  if (poisoned_reason) {
    return;
  }

  ignore_result(asprintf(&why,
"A non-recoverable condition has triggered.  Watchman needs your help!\n"
"The triggering condition was at timestamp=%ld: %s(%.*s) -> %s\n"
"All requests will continue to fail with this message until you resolve\n"
"the underlying problem.  You will find more information on fixing this at\n"
"%s#poison-%s\n",
    (long)now.tv_sec,
    syscall,
    dir->len,
    dir->buf,
    reason ? reason : strerror(err),
    cfg_get_trouble_url(),
    syscall
  ));

  w_log(W_LOG_ERR, "%s", why);

  // This assignment can race for store with other threads.  We don't
  // care about that; we consider ourselves broken and the worst case
  // is that we leak a handful of strings around the race
  poisoned_reason = why;
}

static void crawler(w_root_t *root, struct watchman_pending_collection *coll,
    w_string_t *dir_name, struct timeval now, bool recursive)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  struct watchman_dir_handle *osdir;
  struct watchman_dir_ent *dirent;
  w_ht_iter_t i;
  char path[WATCHMAN_NAME_MAX];
  bool stat_all = false;

  if (root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
    stat_all = root->watcher_ops->flags & WATCHER_COALESCED_RENAME;
  } else {
    // If the watcher doesn't give us per-file notifications for
    // watched dirs, then we'll end up explicitly tracking them
    // and will get updates for the files explicitly.
    // We don't need to look at the files again when we crawl
    stat_all = false;
  }

  dir = w_root_resolve_dir(root, dir_name, true);

  memcpy(path, dir_name->buf, dir_name->len);
  path[dir_name->len] = 0;

  w_log(W_LOG_DBG, "opendir(%s) recursive=%s\n",
      path, recursive ? "true" : "false");

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  osdir = root->watcher_ops->root_start_watch_dir(root, dir, now, path);
  if (!osdir) {
    return;
  }

  if (!dir->files) {
    // Pre-size our hash(es) if we can, so that we can avoid collisions
    // and re-hashing during initial crawl
    uint32_t num_dirs = 0;
#ifndef _WIN32
    struct stat st;
    int dfd = w_dir_fd(osdir);
    if (dfd != -1 && fstat(dfd, &st) == 0) {
      num_dirs = (uint32_t)st.st_nlink;
    }
#endif
    // st.st_nlink is usually number of dirs + 2 (., ..).
    // If it is less than 2 then it doesn't follow that convention.
    // We just pass it through for the dir size hint and the hash
    // table implementation will round that up to the next power of 2
    apply_dir_size_hint(dir, num_dirs, (uint32_t)cfg_get_int(
                                           root, "hint_num_files_per_dir", 64));
  }

  /* flag for delete detection */
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists) {
      file->maybe_deleted = true;
    }
  } while (w_ht_next(dir->files, &i));

  while ((dirent = w_dir_read(osdir)) != NULL) {
    w_string_t *name;

    // Don't follow parent/self links
    if (dirent->d_name[0] == '.' && (
          !strcmp(dirent->d_name, ".") ||
          !strcmp(dirent->d_name, "..")
        )) {
      continue;
    }

    // Queue it up for analysis if the file is newly existing
    name = w_string_new_typed(dirent->d_name, W_STRING_BYTE);
    if (dir->files) {
      file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(name)));
    } else {
      file = NULL;
    }
    if (file) {
      file->maybe_deleted = false;
    }
    if (!file || !file->exists || stat_all || recursive) {
      w_string_t *full_path = w_string_path_cat_cstr(dir->path,
                                dirent->d_name);
      if (full_path) {
        w_log(W_LOG_DBG, "in crawler[%.*s], calling process_path on %.*s\n",
            dir->path->len, dir->path->buf,
            full_path->len, full_path->buf);
        w_root_process_path(root, coll, full_path, now,
                                ((recursive || !file || !file->exists)
                                     ? W_PENDING_RECURSIVE
                                     : 0),
                            dirent);
        w_string_delref(full_path);
      } else {
        w_log(W_LOG_ERR, "OOM during crawl\n");
      }
    }
    w_string_delref(name);
  }
  w_dir_close(osdir);

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists && (file->maybe_deleted ||
          (S_ISDIR(file->stat.mode) && recursive))) {
      w_pending_coll_add_rel(coll, dir, file->name->buf,
          now, recursive ? W_PENDING_RECURSIVE : 0);
    }
  } while (w_ht_next(dir->files, &i));
}

static bool vcs_file_exists(w_root_t *root,
    const char *dname, const char *fname)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  w_string_t *file_name;
  w_string_t *dir_name;
  w_string_t *rel_dir_name;

  rel_dir_name = w_string_new_typed(dname, W_STRING_BYTE);
  dir_name = w_string_path_cat(root->root_path, rel_dir_name);
  w_string_delref(rel_dir_name);

  dir = w_root_resolve_dir(root, dir_name, false);
  w_string_delref(dir_name);

  if (!dir) {
    return false;
  }

  if (!dir->files) {
    return false;
  }

  file_name = w_string_new_typed(fname, W_STRING_BYTE);
  file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
  w_string_delref(file_name);

  if (!file) {
    return false;
  }

  return file->exists;
}

static bool is_vcs_op_in_progress(w_root_t *root) {
  return vcs_file_exists(root, ".hg", "wlock") ||
         vcs_file_exists(root, ".git", "index.lock");
}

static void process_subscriptions(w_root_t *root)
{
  w_ht_iter_t iter;
  bool vcs_in_progress;

  pthread_mutex_lock(&w_client_lock);

  if (!w_ht_first(clients, &iter)) {
    // No subscribers
    goto done;
  }

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer subscription
  // notifications until things settle down
  vcs_in_progress = is_vcs_op_in_progress(root);

  do {
    struct watchman_user_client *client = w_ht_val_ptr(iter.value);
    w_ht_iter_t citer;

    if (w_ht_first(client->subscriptions, &citer)) do {
      struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);
      bool defer = false;
      bool drop = false;

      if (sub->root != root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }
      w_log(W_LOG_DBG, "client->stm=%p sub=%p %s, last=%" PRIu32
          " pending=%" PRIu32 "\n",
          client->client.stm, sub, sub->name->buf, sub->last_sub_tick,
          root->pending_sub_tick);

      if (sub->last_sub_tick == root->pending_sub_tick) {
        continue;
      }

      if (root->asserted_states && w_ht_size(root->asserted_states) > 0
          && sub->drop_or_defer) {
        w_ht_iter_t policy_iter;
        w_string_t *policy_name = NULL;

        // There are 1 or more states asserted and this subscription
        // has some policy for states.  Figure out what we should do.
        if (w_ht_first(sub->drop_or_defer, &policy_iter)) do {
          w_string_t *name = w_ht_val_ptr(policy_iter.key);
          bool policy_is_drop = policy_iter.value;

          if (!w_ht_get(root->asserted_states, policy_iter.key)) {
            continue;
          }

          if (!defer) {
            // This policy is active
            defer = true;
            policy_name = name;
          }

          if (policy_is_drop) {
            drop = true;

            // If we're dropping, we don't need to look at any
            // other policies
            policy_name = name;
            break;
          }
          // Otherwise keep looking until we find a drop
        } while (w_ht_next(sub->drop_or_defer, &policy_iter));

        if (drop) {
          // fast-forward over any notifications while in the drop state
          sub->last_sub_tick = root->pending_sub_tick;
          w_log(W_LOG_DBG, "dropping subscription notifications for %s "
              "until state %s is vacated\n", sub->name->buf, policy_name->buf);
          continue;
        }

        if (defer) {
          w_log(W_LOG_DBG, "deferring subscription notifications for %s "
              "until state %s is vacated\n", sub->name->buf, policy_name->buf);
          continue;
        }
      }

      if (sub->vcs_defer && vcs_in_progress) {
        w_log(W_LOG_DBG, "deferring subscription notifications for %s "
          "until VCS operations complete\n", sub->name->buf);
        continue;
      }

      w_run_subscription_rules(client, sub, root);
      sub->last_sub_tick = root->pending_sub_tick;

    } while (w_ht_next(client->subscriptions, &citer));

  } while (w_ht_next(clients, &iter));
done:
  pthread_mutex_unlock(&w_client_lock);
}

/* process any pending triggers.
 * must be called with root locked
 */
static void process_triggers(w_root_t *root)
{
  w_ht_iter_t iter;

  if (root->last_trigger_tick == root->pending_trigger_tick) {
    return;
  }

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer triggers until
  // things settle down
  if (is_vcs_op_in_progress(root)) {
    w_log(W_LOG_DBG, "deferring triggers until VCS operations complete\n");
    return;
  }

  w_log(W_LOG_DBG, "last=%" PRIu32 "  pending=%" PRIu32 "\n",
      root->last_trigger_tick,
      root->pending_trigger_tick);

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd = w_ht_val_ptr(iter.value);

    if (cmd->current_proc) {
      // Don't spawn if there's one already running
      w_log(W_LOG_DBG, "process_triggers: %.*s is already running\n",
          cmd->triggername->len, cmd->triggername->buf);
      continue;
    }

    w_assess_trigger(root, cmd);

  } while (w_ht_next(root->commands, &iter));

  root->last_trigger_tick = root->pending_trigger_tick;
}

static bool handle_should_recrawl(w_root_t *root)
{
  if (root->should_recrawl && !root->cancelled) {
    char *errmsg;
    // be careful, this is a bit of a switcheroo
    w_root_teardown(root);
    if (!w_root_init(root, &errmsg)) {
      w_log(W_LOG_ERR, "failed to init root %.*s, cancelling watch: %s\n",
          root->root_path->len, root->root_path->buf, errmsg);
      // this should cause us to exit from the notify loop
      w_root_cancel(root);
    }
    root->recrawl_count++;
    if (!root->watcher_ops->root_start(root)) {
      w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
          root->root_path->len, root->root_path->buf,
          root->failure_reason->len, root->failure_reason->buf);
      w_root_cancel(root);
    }
    w_pending_coll_ping(&root->pending);
    return true;
  }
  return false;
}

static bool wait_for_notify(w_root_t *root, int timeoutms)
{
  return root->watcher_ops->root_wait_notify(root, timeoutms);
}

static bool consume_notify(w_root_t *root,
    struct watchman_pending_collection *coll)
{
  return root->watcher_ops->root_consume_notify(root, coll);
}

static void free_file_node(w_root_t *root, struct watchman_file *file)
{
  root->watcher_ops->file_free(file);
  w_string_delref(file->name);
  if (file->symlink_target) {
    w_string_delref(file->symlink_target);
  }
  free(file);
}

static void record_aged_out_dir(w_root_t *root, w_ht_t *aged_dir_names,
    struct watchman_dir *dir)
{
  w_ht_iter_t i;

  w_log(W_LOG_DBG, "age_out: remember dir %.*s\n",
      dir->path->len, dir->path->buf);
  w_ht_insert(aged_dir_names, w_ht_ptr_val(dir->path),
        w_ht_ptr_val(dir), false);

  if (dir->dirs && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    record_aged_out_dir(root, aged_dir_names, child);
    w_ht_iter_del(dir->dirs, &i);
  } while (w_ht_next(dir->dirs, &i));
}

static void age_out_file(w_root_t *root, w_ht_t *aged_dir_names,
    struct watchman_file *file)
{
  struct watchman_dir *dir;
  w_string_t *full_name;

  // Revise tick for fresh instance reporting
  root->last_age_out_tick = MAX(root->last_age_out_tick, file->otime.ticks);

  // And remove from the overall file list
  remove_from_file_list(root, file);
  remove_from_suffix_list(root, file);

  full_name = w_string_path_cat(file->parent->path, file->name);

  if (file->parent->files) {
    // Remove the entry from the containing file hash
    w_ht_del(file->parent->files, w_ht_ptr_val(file->name));
  }
  if (file->parent->dirs) {
    // Remove the entry from the containing dir hash
    w_ht_del(file->parent->dirs, w_ht_ptr_val(full_name));
  }

  // resolve the dir of the same name and mark it for later removal
  // from our internal datastructures
  dir = w_root_resolve_dir(root, full_name, false);
  if (dir) {
    record_aged_out_dir(root, aged_dir_names, dir);
  }

  // And free it.  We don't need to stop watching it, because we already
  // stopped watching it when we marked it as !exists
  free_file_node(root, file);

  w_string_delref(full_name);
}

static void age_out_dir(w_root_t *root, struct watchman_dir *dir)
{
  w_log(W_LOG_DBG, "age_out: ht_del dir %.*s\n",
      dir->path->len, dir->path->buf);

  assert(!dir->files || w_ht_size(dir->files) == 0);

  // This will implicitly call delete_dir() which will tear down
  // the files and dirs hashes
  w_ht_del(root->dirname_to_dir, w_ht_ptr_val(dir->path));
}

// Find deleted nodes older than the gc_age setting.
// This is particularly useful in cases where your tree observes a
// large number of creates and deletes for many unique filenames in
// a given dir (eg: temporary/randomized filenames generated as part
// of build tooling or atomic renames)
void w_root_perform_age_out(w_root_t *root, int min_age)
{
  struct watchman_file *file, *tmp;
  time_t now;
  w_ht_iter_t i;
  w_ht_t *aged_dir_names;

  time(&now);
  root->last_age_out_timestamp = now;
  aged_dir_names = w_ht_new(2, &w_ht_string_funcs);

  file = root->latest_file;
  while (file) {
    if (file->exists || file->otime.tv.tv_sec + min_age > now) {
      file = file->next;
      continue;
    }

    // Get the next file before we remove the current one
    tmp = file->next;

    w_log(W_LOG_DBG, "age_out file=%.*s%c%.*s\n",
        file->parent->path->len, file->parent->path->buf,
        WATCHMAN_DIR_SEP,
        file->name->len, file->name->buf);

    age_out_file(root, aged_dir_names, file);

    file = tmp;
  }

  // For each dir that matched a pruned file node, delete from
  // our internal structures
  if (w_ht_first(aged_dir_names, &i)) do {
    struct watchman_dir *dir = w_ht_val_ptr(i.value);

    age_out_dir(root, dir);
  } while (w_ht_next(aged_dir_names, &i));
  w_ht_free(aged_dir_names);

  // Age out cursors too.
  if (w_ht_first(root->cursors, &i)) do {
    if (i.value < root->last_age_out_tick) {
      w_ht_iter_del(root->cursors, &i);
    }
  } while (w_ht_next(root->cursors, &i));
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

static void consider_age_out(w_root_t *root)
{
  time_t now;

  if (root->gc_interval == 0) {
    return;
  }

  time(&now);

  if (now <= root->last_age_out_timestamp + root->gc_interval) {
    // Don't check too often
    return;
  }

  w_root_perform_age_out(root, root->gc_age);
}

// This is a little tricky.  We have to be called with root->lock
// held, but we must not call w_root_stop_watch with the lock held,
// so we return true if the caller should do that
static bool consider_reap(w_root_t *root) {
  time_t now;

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

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
static void notify_thread(w_root_t *root)
{
  struct watchman_pending_collection pending;

  if (!w_pending_coll_init(&pending)) {
    w_root_cancel(root);
    return;
  }

  if (!root->watcher_ops->root_start(root)) {
    w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
        root->root_path->len, root->root_path->buf,
        root->failure_reason->len, root->failure_reason->buf);
    w_root_cancel(root);
    w_pending_coll_destroy(&pending);
    return;
  }

  // signal that we're done here, so that we can start the
  // io thread after this point
  w_pending_coll_lock(&root->pending);
  root->pending.pinged = true;
  w_pending_coll_ping(&root->pending);
  w_pending_coll_unlock(&root->pending);

  while (!root->cancelled) {
    // big number because not all watchers can deal with
    // -1 meaning infinite wait at the moment
    if (wait_for_notify(root, 86400)) {
      while (consume_notify(root, &pending)) {
        if (w_pending_coll_size(&pending) >= WATCHMAN_BATCH_LIMIT) {
          break;
        }
        if (!wait_for_notify(root, 0)) {
          break;
        }
      }
      if (w_pending_coll_size(&pending) > 0) {
        w_pending_coll_lock(&root->pending);
        w_pending_coll_append(&root->pending, &pending);
        w_pending_coll_ping(&root->pending);
        w_pending_coll_unlock(&root->pending);
      }
    }

    w_root_lock(root, "notify_thread: handle_should_recrawl");
    handle_should_recrawl(root);
    w_root_unlock(root);
  }

  w_pending_coll_destroy(&pending);
}

static void io_thread(w_root_t *root)
{
  int timeoutms, biggest_timeout;
  struct watchman_pending_collection pending;

  timeoutms = root->trigger_settle;

  // Upper bound on sleep delay.  These options are measured in seconds.
  biggest_timeout = root->gc_interval;
  if (biggest_timeout == 0 ||
      (root->idle_reap_age != 0 && root->idle_reap_age < biggest_timeout)) {
    biggest_timeout = root->idle_reap_age;
  }
  if (biggest_timeout == 0) {
    biggest_timeout = 86400;
  }
  // And convert to milliseconds
  biggest_timeout *= 1000;

  w_pending_coll_init(&pending);

  while (!root->cancelled) {
    bool pinged;

    if (!root->done_initial) {
      struct timeval start;
      w_perf_t sample;

      w_perf_start(&sample, "full-crawl");

      /* first order of business is to find all the files under our root */
      if (cfg_get_bool(root, "iothrottle", false)) {
        w_ioprio_set_low();
      }
      w_root_lock(root, "io_thread: bump ticks");
      // Ensure that we observe these files with a new, distinct clock,
      // otherwise a fresh subscription established immediately after a watch
      // can get stuck with an empty view until another change is observed
      root->ticks++;
      gettimeofday(&start, NULL);
      w_pending_coll_add(&root->pending, root->root_path, start, 0);
      // There is the potential for a subtle race condition here.  The boolean
      // parameter indicates whether we want to merge in the set of
      // notifications pending from the watcher or not.  Since we now coalesce
      // overlaps we must consume our outstanding set before we merge in any
      // new kernel notification information or we risk missing out on
      // observing changes that happen during the initial crawl.  This
      // translates to a two level loop; the outer loop sweeps in data from
      // inotify, then the inner loop processes it and any dirs that we pick up
      // from recursive processing.
      while (w_root_process_pending(root, &pending, true)) {
        while (w_root_process_pending(root, &pending, false)) {
          ;
        }
      }
      root->done_initial = true;
      w_perf_add_root_meta(&sample, root);
      w_root_unlock(root);

      if (cfg_get_bool(root, "iothrottle", false)) {
        w_ioprio_set_normal();
      }

      w_perf_finish(&sample);
      w_perf_force_log(&sample);
      w_perf_log(&sample);
      w_perf_destroy(&sample);

      w_log(W_LOG_ERR, "%scrawl complete\n", root->recrawl_count ? "re" : "");
      timeoutms = root->trigger_settle;
    }

    // Wait for the notify thread to give us pending items, or for
    // the settle period to expire
    w_log(W_LOG_DBG, "poll_events timeout=%dms\n", timeoutms);
    pinged = w_pending_coll_lock_and_wait(&root->pending, timeoutms);
    w_log(W_LOG_DBG, " ... wake up (pinged=%s)\n", pinged ? "true" : "false");
    w_pending_coll_append(&pending, &root->pending);
    w_pending_coll_unlock(&root->pending);

    if (!pinged && w_pending_coll_size(&pending) == 0) {
      // No new pending items were given to us, so consider that
      // we may now be settled.

      w_root_lock(root, "io_thread: settle out");
      if (!root->done_initial) {
        // we need to recrawl, stop what we're doing here
        w_root_unlock(root);
        continue;
      }

      process_subscriptions(root);
      process_triggers(root);
      if (consider_reap(root)) {
        w_root_unlock(root);
        w_root_stop_watch(root);
        break;
      }
      consider_age_out(root);
      w_root_unlock(root);

      timeoutms = MIN(biggest_timeout, timeoutms * 2);
      continue;
    }

    // Otherwise we have pending items to stat and crawl

    // We are now, by definition, unsettled, so reduce sleep timeout
    // to the settle duration ready for the next loop through
    timeoutms = root->trigger_settle;

    w_root_lock(root, "io_thread: process notifications");
    if (!root->done_initial) {
      // we need to recrawl.  Discard these notifications
      w_pending_coll_drain(&pending);
      w_root_unlock(root);
      continue;
    }

    root->ticks++;
    // If we're not settled, we need an opportunity to age out
    // dead file nodes.  This happens in the test harness.
    consider_age_out(root);

    while (w_root_process_pending(root, &pending, false)) {
      ;
    }

    w_root_unlock(root);
  }

  w_pending_coll_destroy(&pending);
}

void w_root_addref(w_root_t *root)
{
  w_refcnt_add(&root->refcnt);
}

static void w_root_teardown(w_root_t *root)
{
  struct watchman_file *file;

  if (root->watcher_ops) {
    root->watcher_ops->root_dtor(root);
  }

  if (root->dirname_to_dir) {
    w_ht_free(root->dirname_to_dir);
    root->dirname_to_dir = NULL;
  }
  w_pending_coll_drain(&root->pending);

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

void w_root_delref(w_root_t *root)
{
  if (!w_refcnt_del(&root->refcnt)) return;

  w_log(W_LOG_DBG, "root: final ref on %s\n",
      root->root_path->buf);
  w_cancel_subscriptions_for_root(root);

  w_root_teardown(root);

  pthread_mutex_destroy(&root->lock);
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

  free(root);
  w_refcnt_del(&live_roots);
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

// Must not be called with root->lock held :-/
static bool remove_root_from_watched(w_root_t *root)
{
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

static inline bool is_slash(char c) {
  return (c == '/') || (c == '\\');
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

bool w_is_path_absolute(const char *path) {
#ifdef _WIN32
  char drive_letter;
  size_t len = strlen(path);

  if (len <= 2) {
    return false;
  }

  // "\something"
  if (is_slash(path[0])) {
    // "\\something" is absolute, "\something" is relative to the current
    // dir of the current drive, whatever that may be, for a given process
    return is_slash(path[1]);
  }

  drive_letter = (char)tolower(path[0]);
  // "C:something"
  if (drive_letter >= 'a' && drive_letter <= 'z' && path[1] == ':') {
    // "C:\something" is absolute, but "C:something" is relative to
    // the current dir on the C drive(!)
    return is_slash(path[2]);
  }
  // we could check for things like NUL:, COM: and so on here.
  // While those are technically absolute names, we can't watch them, so
  // we don't consider them absolute for the purposes of checking whether
  // the path is a valid watchable root
  return false;
#else
  return path[0] == '/';
#endif
}

static w_root_t *root_resolve(const char *filename, bool auto_watch,
    bool *created, char **errmsg)
{
  struct watchman_root *root = NULL, *existing = NULL;
  w_ht_val_t root_val;
  char *watch_path;
  w_string_t *root_str;
  int realpath_err;

  *created = false;

  // Sanity check that the path is absolute
  if (!w_is_path_absolute(filename)) {
    ignore_result(asprintf(errmsg, "path \"%s\" must be absolute", filename));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return NULL;
  }

  if (!strcmp(filename, "/")) {
    ignore_result(asprintf(errmsg, "cannot watch \"/\""));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return NULL;
  }

  watch_path = w_realpath(filename);

  struct stat st;
  w_lstat(filename, &st, true);
  if (errno == ENOENT) {
    ignore_result(asprintf(errmsg, "casing problem in \"%s\"", filename));
    w_log(W_LOG_ERR, "resolve_root: %s", *errmsg);
    return NULL;
  }

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
    return NULL;
  }

  if (root || !auto_watch) {
    if (!root) {
      ignore_result(asprintf(errmsg,
            "directory %s is not watched", watch_path));
      w_log(W_LOG_DBG, "resolve_root: %s\n", *errmsg);
    }
    if (watch_path != filename) {
      free(watch_path);
    }

    // Treat this as new activity for aging purposes; this roughly maps
    // to a client querying something about the root and should extend
    // the lifetime of the root
    if (root) {
      w_root_lock(root, "root_resolve: last_cmd_timestamp");
      time(&root->last_cmd_timestamp);
      w_root_unlock(root);
    }

    // caller owns a ref
    return root;
  }

  w_log(W_LOG_DBG, "Want to watch %s -> %s\n", filename, watch_path);

  if (!check_allowed_fs(watch_path, errmsg)) {
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    if (watch_path != filename) {
      free(watch_path);
    }
    return NULL;
  }

  if (!root_check_restrict(watch_path)) {
    ignore_result(asprintf(errmsg,
          "none of the files listed in global config root_files are "
          "present and enforce_root_files is set to true"));
    w_log(W_LOG_ERR, "resolve_root: %s\n", *errmsg);
    if (watch_path != filename) {
      free(watch_path);
    }
    return NULL;
  }

  // created with 1 ref
  root = w_root_new(watch_path, errmsg);

  if (watch_path != filename) {
    free(watch_path);
  }

  if (!root) {
    return NULL;
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
  return root;
}

static void *run_notify_thread(void *arg)
{
  w_root_t *root = arg;

  w_set_thread_name("notify %.*s", root->root_path->len, root->root_path->buf);
  notify_thread(root);

  w_log(W_LOG_DBG, "out of loop\n");

  /* we'll remove it from watched roots if it isn't
   * already out of there */
  remove_root_from_watched(root);

  w_root_delref(root);
  return 0;
}

static void *run_io_thread(void *arg)
{
  w_root_t *root = arg;

  w_set_thread_name("io %.*s", root->root_path->len, root->root_path->buf);
  io_thread(root);
  w_log(W_LOG_DBG, "out of loop\n");

  w_root_delref(root);
  return 0;
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

w_root_t *w_root_resolve_for_client_mode(const char *filename, char **errmsg)
{
  struct watchman_root *root;
  bool created = false;

  root = root_resolve(filename, true, &created, errmsg);
  if (created) {
    struct timeval start;
    struct watchman_pending_collection pending;

    w_pending_coll_init(&pending);

    /* force a walk now */
    gettimeofday(&start, NULL);
    w_root_lock(root, "w_root_resolve_for_client_mode");
    w_pending_coll_add(&root->pending, root->root_path,
        start, W_PENDING_RECURSIVE);
    while (w_root_process_pending(root, &pending, true)) {
      // Note that we don't need a two-level loop (as we do in the main
      // watcher-enabled mode) in client mode as we are not using a
      // watcher in this situation.
      ;
    }
    w_root_unlock(root);

    w_pending_coll_destroy(&pending);
  }
  return root;
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
// Caller must have locked root
bool w_root_cancel(w_root_t *root)
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

bool w_root_stop_watch(w_root_t *root)
{
  bool stopped = remove_root_from_watched(root);

  if (stopped) {
    w_root_cancel(root);
    w_state_save();
  }
  signal_root_threads(root);

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

w_root_t *w_root_resolve(const char *filename, bool auto_watch, char **errmsg)
{
  struct watchman_root *root;
  bool created = false;

  root = root_resolve(filename, auto_watch, &created, errmsg);
  if (created) {
    if (!root_start(root, errmsg)) {
      w_root_cancel(root);
      w_root_delref(root);
      return NULL;
    }
    w_state_save();
  }
  return root;
}

// Caller must have locked root
json_t *w_root_trigger_list_to_json(w_root_t *root)
{
  w_ht_iter_t iter;
  json_t *arr;

  arr = json_array();
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd = w_ht_val_ptr(iter.value);

    json_array_append(arr, cmd->definition);
  } while (w_ht_next(root->commands, &iter));

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
    w_root_t *root;
    bool created = false;
    const char *filename;
    json_t *triggers;
    size_t j;
    char *errmsg = NULL;

    triggers = json_object_get(obj, "triggers");
    filename = json_string_value(json_object_get(obj, "path"));
    root = root_resolve(filename, true, &created, &errmsg);

    if (!root) {
      free(errmsg);
      continue;
    }

    w_root_lock(root, "w_root_load_state");

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

      cmd = w_build_trigger_from_def(root, tobj, &errmsg);
      if (!cmd) {
        w_log(W_LOG_ERR, "loading trigger for %s: %s\n",
            root->root_path->buf, errmsg);
        free(errmsg);
        continue;
      }

      w_ht_replace(root->commands, w_ht_ptr_val(cmd->triggername),
          w_ht_ptr_val(cmd));
    }

    w_root_unlock(root);

    if (created) {
      if (!root_start(root, &errmsg)) {
        w_log(W_LOG_ERR, "root_start(%s) failed: %s\n",
            root->root_path->buf, errmsg);
        free(errmsg);
        w_root_cancel(root);
      }
    }

    w_root_delref(root);
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
    w_root_t *root = w_ht_val_ptr(root_iter.value);
    json_t *obj;
    json_t *triggers;

    obj = json_object();

    json_object_set_new(obj, "path", w_string_to_json(root->root_path));

    w_root_lock(root, "w_root_save_state");
    triggers = w_root_trigger_list_to_json(root);
    w_root_unlock(root);
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
