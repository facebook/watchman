/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

static struct watchman_ops *watcher_ops = NULL;
static watchman_global_watcher_t watcher = NULL;
static w_ht_t *watched_roots = NULL;
static int live_roots = 0;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static uint32_t next_root_number = 1;

/* Some error conditions will put us into a non-recoverable state where we
 * can't guarantee that we will be operating correctly.  Rather than suffering
 * in silence and misleading our clients, we'll poison ourselves and advertise
 * that we have done so and provide some advice on how the user can cure us. */
char *poisoned_reason = NULL;

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool recursive);

static void w_root_teardown(w_root_t *root);

static void free_pending(struct watchman_pending_fs *p)
{
  w_string_delref(p->path);
  free(p);
}

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

  w_string_delref(dir->path);
  if (dir->files) {
    w_ht_free(dir->files);
  }
  if (dir->lc_files) {
    w_ht_free(dir->lc_files);
  }
  if (dir->dirs) {
    w_ht_free(dir->dirs);
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

  snprintf(cfgfilename, sizeof(cfgfilename), "%s/.watchmanconfig", path);

  if (access(cfgfilename, R_OK) != 0) {
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

  memset((char *)root + root_init_offset, 0,
         sizeof(w_root_t) - root_init_offset);

  if (!watcher_ops->root_init(watcher, root, errmsg)) {
    return false;
  }

  root->number = __sync_fetch_and_add(&next_root_number, 1);

  root->cursors = w_ht_new(2, &w_ht_string_funcs);
  root->suffixes = w_ht_new(2, &w_ht_string_funcs);
  root->pending_uniq = w_ht_new(WATCHMAN_BATCH_LIMIT, &w_ht_string_funcs);

  root->dirname_to_dir = w_ht_new(HINT_NUM_DIRS, &dirname_hash_funcs);
  root->ticks = 1;

  // "manually" populate the initial dir, as the dir resolver will
  // try to find its parent and we don't want it to for the root
  dir = calloc(1, sizeof(*dir));
  dir->path = root->root_path;
  dir->wd = -1;
  w_string_addref(dir->path);
  w_ht_set(root->dirname_to_dir, w_ht_ptr_val(dir->path), w_ht_ptr_val(dir));

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
    const char *ignore = json_string_value(json_array_get(ignores, i));

    if (!ignore) {
      ignore_result(asprintf(errmsg,
          "ignore_vcs must be an array of strings"));
      json_decref(ignores);
      return false;
    }

    name = w_string_new(ignore);
    fullname = w_string_path_cat(root->root_path, name);

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (w_ht_get(root->ignore_dirs, w_ht_ptr_val(fullname))) {
      w_string_delref(fullname);
      w_string_delref(name);
      continue;
    }

    w_ht_set(root->ignore_vcs, w_ht_ptr_val(fullname),
        w_ht_ptr_val(fullname));

    // While we're at it, see if we can find out where to put our
    // query cookie information
    if (root->query_cookie_dir == NULL &&
        lstat(fullname->buf, &st) == 0 && S_ISDIR(st.st_mode)) {
      // root/{.hg,.git,.svn}
      root->query_cookie_dir = w_string_path_cat(root->root_path, name);
    }
    w_string_delref(name);
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
      "%.*s/" WATCHMAN_COOKIE_PREFIX "%s-%d-", root->query_cookie_dir->len,
      root->query_cookie_dir->buf, hostname, (int)getpid());
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
    const char *ignore = json_string_value(json_array_get(ignores, i));

    if (!ignore) {
      w_log(W_LOG_ERR, "ignore_dirs must be an array of strings\n");
      continue;
    }

    name = w_string_new(ignore);
    fullname = w_string_path_cat(root->root_path, name);
    w_ht_set(root->ignore_dirs, w_ht_ptr_val(fullname),
        w_ht_ptr_val(fullname));
    w_log(W_LOG_DBG, "ignoring %.*s recursively\n",
        fullname->len, fullname->buf);
    w_string_delref(fullname);
    w_string_delref(name);
  }
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

#ifdef __APPLE__
  root->case_sensitive = pathconf(path, _PC_CASE_SENSITIVE);
#else
  root->case_sensitive = true;
#endif

  root->root_path = w_string_new(path);
  root->commands = w_ht_new(2, &trigger_hash_funcs);
  root->query_cookies = w_ht_new(2, &w_ht_string_funcs);
  root->ignore_vcs = w_ht_new(2, &w_ht_string_funcs);
  root->ignore_dirs = w_ht_new(2, &w_ht_string_funcs);

  load_root_config(root, path);
  root->trigger_settle = cfg_get_int(root, "settle", DEFAULT_SETTLE_PERIOD);
  root->gc_age = cfg_get_int(root, "gc_age_seconds", DEFAULT_GC_AGE);
  root->gc_interval = cfg_get_int(root, "gc_interval_seconds",
      DEFAULT_GC_INTERVAL);

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

void w_root_lock(w_root_t *root)
{
  int err;

  err = pthread_mutex_lock(&root->lock);
  if (err != 0) {
    w_log(W_LOG_FATAL, "lock [%.*s]: %s\n",
        root->root_path->len,
        root->root_path->buf,
        strerror(err)
    );
  }
}

void w_root_unlock(w_root_t *root)
{
  int err;

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
  int fd;
  int errcode = 0;
  struct timespec deadline;
  struct timeval now, delta, target;

  if (pthread_cond_init(&cookie.cond, NULL)) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: cond_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }
  cookie.seen = false;

  /* generate a cookie name: cookie prefix + id */
  w_root_lock(root);
  tick = root->ticks++;
  path_str = w_string_make_printf("%.*s%" PRIu32 "-%" PRIu32,
                                  root->query_cookie_prefix->len,
                                  root->query_cookie_prefix->buf,
                                  root->number, tick);
  /* insert our cookie in the map */
  w_ht_set(root->query_cookies, w_ht_ptr_val(path_str),
      w_ht_ptr_val(&cookie));

  /* touch the file */
  fd = open(path_str->buf, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0700);
  if (fd == -1) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: creat(%s) failed: %s\n",
        path_str->buf, strerror(errcode));
    goto out;
  }
  close(fd);

  /* compute deadline */
  gettimeofday(&now, NULL);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, &deadline);

  w_log(W_LOG_DBG, "sync_to_now [%s] waiting\n", path_str->buf);

  /* timed cond wait (unlocks root lock, reacquires) */
  errcode = pthread_cond_timedwait(&cookie.cond, &root->lock, &deadline);
  if (errcode && !cookie.seen) {
    w_log(W_LOG_ERR, "sync_to_now: %s timedwait failed: %s\n",
        path_str->buf, strerror(errcode));
  } else {
    w_log(W_LOG_DBG, "sync_to_now [%s] done\n", path_str->buf);
  }

out:
  // can't unlink the file until after the cookie has been observed because
  // we don't know which file got changed until we look in the cookie dir
  unlink(path_str->buf);
  w_ht_del(root->query_cookies, w_ht_ptr_val(path_str));
  w_root_unlock(root);

  w_string_delref(path_str);
  pthread_cond_destroy(&cookie.cond);

  if (!cookie.seen) {
    errno = errcode;
    return false;
  }

  return true;
}

bool w_root_add_pending(w_root_t *root, w_string_t *path,
    bool recursive, struct timeval now, bool via_notify)
{
  struct watchman_pending_fs *p;

  p = w_ht_val_ptr(w_ht_get(root->pending_uniq, w_ht_ptr_val(path)));
  if (p) {
    if (!p->recursive && recursive) {
      p->recursive = true;
    }
    return true;
  }

  p = calloc(1, sizeof(*p));
  if (!p) {
    return false;
  }

  w_log(W_LOG_DBG, "add_pending: %.*s\n", path->len, path->buf);

  p->recursive = recursive;
  p->now = now;
  p->via_notify = via_notify;
  p->path = path;
  w_string_addref(path);

  p->next = root->pending;
  root->pending = p;
  w_ht_set(root->pending_uniq, w_ht_ptr_val(path), w_ht_ptr_val(p));

  return true;
}

bool w_root_add_pending_rel(w_root_t *root, struct watchman_dir *dir,
    const char *name, bool recursive,
    struct timeval now, bool via_notify)
{
  char path[WATCHMAN_NAME_MAX];
  w_string_t *path_str;
  bool res;

  snprintf(path, sizeof(path), "%.*s/%s", dir->path->len, dir->path->buf, name);
  path_str = w_string_new(path);

  res = w_root_add_pending(root, path_str, recursive, now, via_notify);

  w_string_delref(path_str);

  return res;
}

bool w_root_process_pending(w_root_t *root, bool drain)
{
  struct watchman_pending_fs *pending, *p;

  if (!root->pending) {
    return false;
  }

  w_log(W_LOG_DBG, "processing %d events in %s\n",
      w_ht_size(root->pending_uniq), root->root_path->buf);
  w_ht_free_entries(root->pending_uniq);

  pending = root->pending;
  root->pending = NULL;

  while (pending) {
    p = pending;
    pending = p->next;

    if (!drain && !root->cancelled) {
      w_root_process_path(root, p->path, p->now, p->recursive, p->via_notify);
    }

    free_pending(p);
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
  dir->wd = -1;
  w_string_addref(dir->path);

  if (!parent->dirs) {
    parent->dirs = w_ht_new(2, &w_ht_string_funcs);
  }

  assert(w_ht_set(parent->dirs, w_ht_ptr_val(dir_name), w_ht_ptr_val(dir)));
  assert(w_ht_set(root->dirname_to_dir,
        w_ht_ptr_val(dir_name), w_ht_ptr_val(dir)));

  return dir;
}

static void watch_file(w_root_t *root, struct watchman_file *file)
{
  watcher_ops->root_start_watch_file(watcher, root, file);
}

static void stop_watching_file(w_root_t *root, struct watchman_file *file)
{
  watcher_ops->root_stop_watch_file(watcher, root, file);
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

  watcher_ops->root_stop_watch_dir(watcher, root, dir);
}

static bool did_file_change(struct stat *saved, struct stat *fresh)
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
  struct timespec a = saved->WATCHMAN_ST_TIMESPEC(wat); \
  struct timespec b = fresh->WATCHMAN_ST_TIMESPEC(wat); \
  if (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec) { \
    return true; \
  } \
}

  FIELD_CHG(st_mode);

  if (!S_ISDIR(saved->st_mode)) {
    FIELD_CHG(st_size);
    FIELD_CHG(st_nlink);
  }
  FIELD_CHG(st_dev);
  FIELD_CHG(st_ino);
  FIELD_CHG(st_uid);
  FIELD_CHG(st_gid);
  FIELD_CHG(st_rdev);
  // Don't care about st_atime
  FIELD_CHG(st_ctime);
  FIELD_CHG(st_mtime);
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

#ifdef __APPLE__
static w_string_t *w_resolve_filesystem_canonical_name(const char *path)
{
  struct attrlist attrlist;
  struct {
    uint32_t len;
    attrreference_t ref;
    char canonical_name[WATCHMAN_NAME_MAX];
  } vomit;
  char *name;

  memset(&attrlist, 0, sizeof(attrlist));
  attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrlist.commonattr = ATTR_CMN_NAME;

  if (getattrlist(path, &attrlist, &vomit,
        sizeof(vomit), FSOPT_NOFOLLOW) == -1) {
    // signal to caller that the file has disappeared -- the caller will read
    // errno and do error handling
    return NULL;
  }

  name = ((char*)&vomit.ref) + vomit.ref.attr_dataoffset;
  return w_string_new(name);
}
#endif

static void stat_path(w_root_t *root,
    w_string_t *full_path, struct timeval now, bool recursive, bool via_notify)
{
  struct stat st;
  int res, err;
  char path[WATCHMAN_NAME_MAX];
  struct watchman_dir *dir;
  struct watchman_dir *dir_ent = NULL;
  struct watchman_file *file = NULL;
  w_string_t *dir_name;
  w_string_t *file_name;

  if (w_ht_get(root->ignore_dirs, w_ht_ptr_val(full_path))) {
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

  res = lstat(path, &st);
  err = res == 0 ? 0 : errno;
  w_log(W_LOG_DBG, "lstat(%s) file=%p dir=%p\n", path, file, dir_ent);

  if (res && (err == ENOENT || err == ENOTDIR)) {
    /* it's not there, update our state */
    if (dir_ent) {
      w_root_mark_deleted(root, dir_ent, now, true);
      w_log(W_LOG_DBG, "lstat(%s) -> %s so stopping watch on %s\n",
          path, strerror(err), dir_ent->path->buf);
      stop_watching_dir(root, dir_ent);
    }
    if (file) {
      w_log(W_LOG_DBG, "lstat(%s) -> %s so marking %.*s deleted\n",
          path, strerror(err), file->name->len, file->name->buf);
      file->exists = false;
      w_root_mark_file_changed(root, file, now);
    }
  } else if (res) {
    w_log(W_LOG_ERR, "lstat(%s) %d %s\n",
        path, err, strerror(err));
  } else {
    if (!file) {
      file = w_root_resolve_file(root, dir, file_name, now);
    }

#ifdef __APPLE__
    if (!root->case_sensitive) {
      // Determine canonical case from filesystem
      w_string_t *canon_name;
      w_string_t *lc_file_name;
      struct watchman_file *lc_file = NULL;

      canon_name = w_resolve_filesystem_canonical_name(path);

      if (canon_name == NULL) {
        // TOCTOU race: file disappeared (deleted? ran out of memory?) in
        // between the lstat above and w_resolve_filesystem_canonical_name. Now
        // that it's deleted, update our state to reflect that.
        if (errno == ENOENT || errno == ENOTDIR || errno == ENOFOLLOWSYMLINK) {
          if (dir_ent) {
            handle_open_errno(root, dir_ent, now, "getattrlist", errno, NULL);
          }
          if (file) {
            w_log(W_LOG_DBG, "getattrlist(%s) -> %s so marking %.*s deleted\n",
                  path, strerror(err), file->name->len, file->name->buf);
            file->exists = false;
            w_root_mark_file_changed(root, file, now);
          }

          goto out;
        }

        w_log(W_LOG_FATAL, "getattrlist(CMN_NAME: %s): fail %s\n",
              path, strerror(errno));
      }

      if (!w_string_equal(file_name, canon_name)) {
        // Revise `path` to use the canonical name
        // We do this by walking back file_name->len, then adding
        // canon_name on the end.  We can't assume that canon_name
        // and file_name have the same byte length because the case
        // folded representation may potentially have differing
        // byte lengths.  Since the length can change, we need to
        // re-check the resultant length for overflow.
        if (full_path->len - file_name->len + canon_name->len > sizeof(path)-1)
        {
          w_log(W_LOG_FATAL, "canon path %.*s%.*s is too big\n",
            full_path->len - file_name->len, full_path->buf,
            canon_name->len, canon_name->buf);
        }

        snprintf(path, sizeof(path), "%.*s%.*s",
          full_path->len - file_name->len, full_path->buf,
          canon_name->len, canon_name->buf);

        w_log(W_LOG_DBG, "canon -> %s\n", path);

        // file refers to a node that doesn't exist any longer
        file->exists = false;
        w_root_mark_file_changed(root, file, now);

        // Create or resurrect a file node from this canonical name
        file = w_root_resolve_file(root, dir, canon_name, now);
      }

      if (dir_ent) {
        w_string_t *dir_basename = w_string_basename(dir_ent->path);

        if (!w_string_equal(dir_basename, canon_name)) {
          // If the case changed, we logically deleted that part of
          // the tree.  Our clients will expect to see deletes for
          // the tree, followed by notifications of the files at their
          // new canonical path name
          w_log(W_LOG_DBG, "canon(%s) changed on dir, so marking deleted\n",
              path);

          stop_watching_dir(root, dir_ent);
          w_root_mark_deleted(root, dir_ent, now, true);

          // Ensure we recurse and build out the new tree
          recursive = true;
          dir_ent = NULL;
        }
        w_string_delref(dir_basename);
      }

      lc_file_name = w_string_dup_lower(file_name);

      if (!dir->lc_files) {
        dir->lc_files = w_ht_new(2, &w_ht_string_funcs);
      } else {
        lc_file = w_ht_val_ptr(w_ht_get(dir->lc_files,
                      w_ht_ptr_val(lc_file_name)));
        if (lc_file && !w_string_equal(lc_file->name, file->name)) {
          // lc_file is no longer the canonical item, it must have
          // been deleted
          lc_file->exists = false;
          w_root_mark_file_changed(root, lc_file, now);
        }
      }

      // Revise lc_files to reference the latest version of this
      w_ht_replace(dir->lc_files, w_ht_ptr_val(lc_file_name),
                   w_ht_ptr_val(file));
      w_string_delref(lc_file_name);
      lc_file_name = NULL;

      if (!w_string_equal(file_name, canon_name)) {
        // Ensure that we use the canonical name for the remainder
        // of this function call
        w_string_delref(file_name);
        file_name = canon_name;
        canon_name = NULL;

        // We don't delref full_path here, it's owned by the caller
        full_path = w_string_path_cat(dir_name, file_name);
      } else {
        w_string_delref(canon_name);
      }
    }
#endif

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = root->ticks;
      file->ctime.tv = now;
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->st, &st)) {
      w_log(W_LOG_DBG,
          "file changed exists=%d via_notify=%d stat-changed=%d isdir=%d %s\n",
          (int)file->exists,
          (int)via_notify,
          (int)(file->exists && !via_notify),
          S_ISDIR(st.st_mode),
          path
      );
      file->exists = true;
      w_root_mark_file_changed(root, file, now);
    }
    memcpy(&file->st, &st, sizeof(st));
    if (S_ISDIR(st.st_mode)) {
      if (dir_ent == NULL) {
        recursive = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!w_ht_get(root->ignore_vcs, w_ht_ptr_val(dir_name)) ||
          // but do if we're looking at the cookie dir (stat_path is never
          // called for the root itself)
          w_string_equal(full_path, root->query_cookie_dir)) {

        if (!watcher_ops->has_per_file_notifications) {
          /* we always need to crawl, but may not need to be fully recursive */
          crawler(root, full_path, now, recursive);
        } else {
          /* we get told about changes on the child, so we only
           * need to crawl if we've never seen the dir before */
          if (recursive) {
            crawler(root, full_path, now, recursive);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      w_root_mark_deleted(root, dir_ent, now, true);
    }
    if (watcher_ops->has_per_file_notifications && !S_ISDIR(st.st_mode) &&
        !w_string_equal(dir_name, root->root_path)) {
      /* Make sure we update the mtime on the parent directory. */
      stat_path(root, dir_name, now, false, via_notify);
    }
  }

  // out is only used on some platforms, so on others compilers will complain
  // about it being unused
  goto out;

out:
  w_string_delref(dir_name);
  w_string_delref(file_name);
}


void w_root_process_path(w_root_t *root, w_string_t *full_path,
    struct timeval now, bool recursive, bool via_notify)
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
    bool consider_cookie = watcher_ops->has_per_file_notifications ?
      (via_notify || !root->done_initial) : true;

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

  if (w_string_equal(full_path, root->root_path)) {
    crawler(root, full_path, now, recursive);
  } else {
    stat_path(root, full_path, now, recursive, via_notify);
  }
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, bool recursive)
{
  w_ht_iter_t i;

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = w_ht_val_ptr(i.value);

    if (file->exists) {
      w_log(W_LOG_DBG, "mark_deleted: %.*s/%.*s\n",
          dir->path->len, dir->path->buf,
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

/* Opens a directory making sure it's not a symlink */
DIR *opendir_nofollow(const char *path)
{
  int fd = open(path, O_NOFOLLOW | O_CLOEXEC);
  if (fd == -1) {
    return NULL;
  }
#ifdef __APPLE__
  close(fd);
  return opendir(path);
#else
  // errno should be set appropriately if this is not a directory
  return fdopendir(fd);
#endif
}

void handle_open_errno(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, const char *syscall, int err, const char *reason)
{
  w_string_t *dir_name = dir->path;
  if (err == ENOENT || err == ENOTDIR || err == ENOFOLLOWSYMLINK) {
    if (w_string_equal(dir_name, root->root_path)) {
      w_log(W_LOG_ERR,
            "%s(%.*s) -> %s. Root was deleted; cancelling watch\n",
            syscall, dir_name->len, dir_name->buf,
            reason ? reason : strerror(err));
      w_root_cancel(root);
      return;
    }

    w_log(W_LOG_DBG, "%s(%.*s) -> %s so invalidating descriptors\n",
          syscall, dir_name->len, dir_name->buf,
          reason ? reason : strerror(err));
    stop_watching_dir(root, dir);
    w_root_mark_deleted(root, dir, now, true);
    return;
  }
  w_log(W_LOG_ERR, "%s(%.*s) -> %s. We don't know how to handle this.",
        syscall, dir_name->len, dir_name->buf,
        reason ? reason : strerror(err));
}

void set_poison_state(w_root_t *root, struct watchman_dir *dir,
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
"https://facebook.github.io/watchman/docs/troubleshooting.html#poison-%s\n",
    (long)now.tv_sec,
    syscall,
    dir->path->len,
    dir->path->buf,
    reason ? reason : strerror(err),
    syscall
  ));

  w_log(W_LOG_ERR, "%s", why);

  // This assignment can race for store with other threads.  We don't
  // care about that; we consider ourselves broken and the worst case
  // is that we leak a handful of strings around the race
  poisoned_reason = why;
}

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool recursive)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  DIR *osdir;
  struct dirent *dirent;
  w_ht_iter_t i;
  char path[WATCHMAN_NAME_MAX];

  dir = w_root_resolve_dir(root, dir_name, true);

  memcpy(path, dir_name->buf, dir_name->len);
  path[dir_name->len] = 0;

  w_log(W_LOG_DBG, "opendir(%s) recursive=%s\n",
      path, recursive ? "true" : "false");

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  osdir = watcher_ops->root_start_watch_dir(watcher, root, dir, now, path);
  if (!osdir) {
    return;
  }

  /* flag for delete detection */
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists) {
      file->maybe_deleted = true;
    }
  } while (w_ht_next(dir->files, &i));

  while ((dirent = readdir(osdir)) != NULL) {
    w_string_t *name;

    // Don't follow parent/self links
    if (dirent->d_name[0] == '.' && (
          !strcmp(dirent->d_name, ".") ||
          !strcmp(dirent->d_name, "..")
        )) {
      continue;
    }

    // Queue it up for analysis if the file is newly existing
    name = w_string_new(dirent->d_name);
    if (dir->files) {
      file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(name)));
    } else {
      file = NULL;
    }
    if (file) {
      file->maybe_deleted = false;
    }
    if (!file || !file->exists) {
      w_root_add_pending_rel(root, dir, dirent->d_name,
          true, now, false);
    }
    w_string_delref(name);
  }
  closedir(osdir);

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  if (w_ht_first(dir->files, &i)) do {
    file = w_ht_val_ptr(i.value);
    if (file->exists && (file->maybe_deleted ||
          (S_ISDIR(file->st.st_mode) && recursive))) {
      w_root_add_pending_rel(root, dir, file->name->buf,
          recursive, now, false);
    }
  } while (w_ht_next(dir->files, &i));
}

static void process_subscriptions(w_root_t *root)
{
  w_ht_iter_t iter;

  if (root->last_sub_tick == root->pending_sub_tick) {
    return;
  }

  w_log(W_LOG_DBG, "sub last=%" PRIu32 "  pending=%" PRIu32 "\n",
      root->last_sub_tick,
      root->pending_sub_tick);

  /* now look for subscribers */
  w_log(W_LOG_DBG, "looking for connected subscribers\n");
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_client *client = w_ht_val_ptr(iter.value);
    w_ht_iter_t citer;

    w_log(W_LOG_DBG, "client=%p fd=%d\n", client, client->fd);

    if (w_ht_first(client->subscriptions, &citer)) do {
      struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);

      w_log(W_LOG_DBG, "sub=%p %s\n", sub, sub->name->buf);
      if (sub->root != root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }

      w_run_subscription_rules(client, sub, root);

    } while (w_ht_next(client->subscriptions, &citer));

  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

  root->last_sub_tick = root->pending_sub_tick;
}

static bool vcs_file_exists(w_root_t *root,
    const char *dname, const char *fname)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  w_string_t *file_name;
  w_string_t *dir_name;
  w_string_t *rel_dir_name;

  rel_dir_name = w_string_new(dname);
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

  file_name = w_string_new(fname);
  file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
  w_string_delref(file_name);

  if (!file) {
    return false;
  }

  return file->exists;
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
  if (vcs_file_exists(root, ".hg", "wlock") ||
      vcs_file_exists(root, ".git", "index.lock")) {
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
    if (!watcher_ops->root_start(watcher, root)) {
      w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
          root->root_path->len, root->root_path->buf,
          root->failure_reason->len, root->failure_reason->buf);
      w_root_cancel(root);
    }
    return true;
  }
  return false;
}

static bool wait_for_notify(w_root_t *root, int timeoutms)
{
  return watcher_ops->root_wait_notify(watcher, root, timeoutms);
}

static bool consume_notify(w_root_t *root)
{
  return watcher_ops->root_consume_notify(watcher, root);
}

static void free_file_node(struct watchman_file *file)
{
  watcher_ops->file_free(watcher, file);
  w_string_delref(file->name);
  free(file);
}

static void age_out_file(w_root_t *root, struct watchman_file *file);

static void age_out_dir(w_root_t *root, struct watchman_dir *dir)
{
  w_ht_iter_t i;
  // Since age_out_file deletes itself from dir->files, it will
  // invalidate the iterator, so we have to restart our iteration each time.
  while (dir->files && w_ht_first(dir->files, &i)) {
    struct watchman_file *file = w_ht_val_ptr(i.value);

    assert(!file->exists);
    age_out_file(root, file);
  }

  // Since age_out_dir deletes itself from dir->dirs, it will
  // invalidate the iterator, so we have to restart our iteration each time.
  while (dir->dirs && w_ht_first(dir->dirs, &i)) {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    age_out_dir(root, child);
  }

  // This will implicitly call delete_dir() which will tear down
  // the files and dirs hashes
  w_ht_del(root->dirname_to_dir, w_ht_ptr_val(dir->path));
}

static void age_out_file(w_root_t *root, struct watchman_file *file)
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

  // resolve the dir of the same name and recursively clean its
  // contents
  dir = w_root_resolve_dir(root, full_name, false);
  if (dir) {
    age_out_dir(root, dir);
  }

  // And free it.  We don't need to stop watching it, because we already
  // stopped watching it when we marked it as !exists
  free_file_node(file);

  w_string_delref(full_name);
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

  time(&now);
  root->last_age_out_timestamp = now;

  file = root->latest_file;
  while (file) {
    if (file->exists || file->otime.tv.tv_sec + min_age > now) {
      file = file->next;
      continue;
    }

    // We look backwards for the next iteration, as forwards may
    // be a file node that will also be deleted by age_out_file()
    // below because it is a child node of the the current value
    // of file.
    tmp = file->prev;

    w_log(W_LOG_DBG, "age_out file=%.*s/%.*s\n",
        file->parent->path->len, file->parent->path->buf,
        file->name->len, file->name->buf);

    age_out_file(root, file);

    if (tmp) {
      file = tmp;
    } else {
      file = root->latest_file;
    }
  }

  // Age out cursors too.
  if (w_ht_first(root->cursors, &i)) do {
    if (i.value < root->last_age_out_tick) {
      w_ht_iter_del(root->cursors, &i);
    }
  } while (w_ht_next(root->cursors, &i));
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

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work until after
// we have drained the inotify descriptor
static void notify_thread(w_root_t *root)
{
  if (!watcher_ops->root_start(watcher, root)) {
    w_log(W_LOG_ERR, "failed to start root %.*s, cancelling watch: %.*s\n",
        root->root_path->len, root->root_path->buf,
        root->failure_reason->len, root->failure_reason->buf);
    w_root_cancel(root);
    return;
  }

  /* now we can settle into the notification stuff */
  while (!root->cancelled) {
    int timeoutms = MAX(root->trigger_settle, 100);

    if (!root->done_initial) {
      struct timeval start;
      /* first order of business is to find all the files under our root */
      w_root_lock(root);
      gettimeofday(&start, NULL);
      w_root_add_pending(root, root->root_path, false, start, false);
      while (root->pending) {
        w_root_process_pending(root, false);
      }
      root->done_initial = true;
      w_root_unlock(root);

      w_log(W_LOG_DBG, "notify_thread[%s]: initial crawl complete\n",
            root->root_path->buf);
    }

    if (!wait_for_notify(root, timeoutms)) {
      // Do triggers
      w_root_lock(root);
      if (handle_should_recrawl(root)) {
        goto unlock;
      }

      w_log(W_LOG_DBG, "notify_thread[%s] assessing triggers\n",
          root->root_path->buf);
      process_subscriptions(root);
      process_triggers(root);
      consider_age_out(root);
      w_root_unlock(root);
      continue;
    }

    // Otherwise we have stuff to do
    w_root_lock(root);
    // If we're not settled, we need an opportunity to age out
    // dead file nodes.  This happens in the test harness.
    consider_age_out(root);
    root->ticks++;

    // Consume as much as we can without blocking on inotify,
    // because we hold a lock.
    while (!root->cancelled &&
        w_ht_size(root->pending_uniq) < WATCHMAN_BATCH_LIMIT &&
        consume_notify(root) &&
        wait_for_notify(root, 0)) {
      ;
    }

    // then do our IO
    if (handle_should_recrawl(root)) {
      goto unlock;
    }
    while (root->pending) {
      w_root_process_pending(root, false);
    }

    handle_should_recrawl(root);
unlock:
    w_root_unlock(root);
  }

}

/* This function always returns a buffer that needs to
 * be released via free(3).  We use the native feature
 * of the system libc if we know it is present, otherwise
 * we need to malloc a buffer for ourselves.  This
 * is made more fun because some systems have a dynamic
 * buffer size obtained via sysconf().
 */
char *w_realpath(const char *filename)
{
#if defined(__GLIBC__) || defined(__APPLE__)
  return realpath(filename, NULL);
#else
  char *buf = NULL;
  char *retbuf;
  int path_max = 0;

#ifdef _SC_PATH_MAX
  path_max = sysconf(path, _SC_PATH_MAX);
#endif
  if (path_max <= 0) {
    path_max = WATCHMAN_NAME_MAX;
  }
  buf = malloc(path_max);
  if (!buf) {
    return NULL;
  }

  retbuf = realpath(filename, buf);

  if (retbuf != buf) {
    free(buf);
    return NULL;
  }

  return retbuf;
#endif
}

void w_root_addref(w_root_t *root)
{
  w_refcnt_add(&root->refcnt);
}

static void w_root_teardown(w_root_t *root)
{
  struct watchman_file *file;

  watcher_ops->root_dtor(watcher, root);

  if (root->dirname_to_dir) {
    w_ht_free(root->dirname_to_dir);
    root->dirname_to_dir = NULL;
  }
  if (root->pending_uniq) {
    w_ht_free(root->pending_uniq);
    root->pending_uniq = NULL;
  }

  while (root->pending) {
    struct watchman_pending_fs *p;

    p = root->pending;
    root->pending = p->next;
    w_string_delref(p->path);
    free(p);
  }

  while (root->latest_file) {
    file = root->latest_file;
    root->latest_file = file->next;
    free_file_node(file);
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

  w_root_teardown(root);

  pthread_mutex_destroy(&root->lock);
  w_string_delref(root->root_path);
  w_ht_free(root->ignore_vcs);
  w_ht_free(root->ignore_dirs);
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

  if (root->query_cookie_dir) {
    w_string_delref(root->query_cookie_dir);
  }
  if (root->query_cookie_prefix) {
    w_string_delref(root->query_cookie_prefix);
  }

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

#if HAVE_FSEVENTS
  watcher_ops = &fsevents_watcher;
#elif defined(HAVE_PORT_CREATE)
  // We prefer portfs if you have both portfs and inotify on the assumption
  // that this is an Illumos based system with both and that the native
  // mechanism will yield more correct behavior.
  // https://github.com/facebook/watchman/issues/84
  watcher_ops = &portfs_watcher;
#elif defined(HAVE_INOTIFY_INIT)
  watcher_ops = &inotify_watcher;
#elif defined(HAVE_KQUEUE)
  watcher_ops = &kqueue_watcher;
#else
# error you need to assign watcher_ops for this system
#endif

  watcher = watcher_ops->global_init();

  w_log(W_LOG_ERR, "Using watcher mechanism %s\n", watcher_ops->name);
}

void watchman_watcher_dtor(void) {
  watcher_ops->global_dtor(watcher);
}


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
    int rv;

    ignore_result(asprintf(&restrict_path, "%s/%s", watch_path,
                           restrict_file));
    rv = access(restrict_path, F_OK);
    free(restrict_path);
    if (rv == 0)
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

static w_root_t *root_resolve(const char *filename, bool auto_watch,
    bool *created, char **errmsg)
{
  struct watchman_root *root = NULL;
  w_ht_val_t root_val;
  char *watch_path;
  w_string_t *root_str;
  int realpath_err;

  *created = false;

  // Sanity check that the path is absolute
  if (filename[0] != '/') {
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
  realpath_err = errno;

  if (!watch_path) {
    watch_path = (char*)filename;
  }

  root_str = w_string_new(watch_path);
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

  *created = true;

  pthread_mutex_lock(&root_lock);
  // adds 1 ref
  w_ht_set(watched_roots, w_ht_ptr_val(root->root_path), w_ht_ptr_val(root));
  pthread_mutex_unlock(&root_lock);

  // caller owns 1 ref
  return root;
}

static void *run_notify_thread(void *arg)
{
  w_root_t *root = arg;

  notify_thread(root);

  w_log(W_LOG_DBG, "notify_thread: out of loop %s\n",
      root->root_path->buf);

  /* we'll remove it from watched roots if it isn't
   * already out of there */
  remove_root_from_watched(root);

  w_root_delref(root);
  return 0;
}

static bool root_start(w_root_t *root, char **errmsg)
{
  pthread_attr_t attr;
  int err;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  w_root_addref(root);
  err = pthread_create(&root->notify_thread, &attr, run_notify_thread, root);
  pthread_attr_destroy(&attr);

  if (err == 0) {
    return true;
  }

  ignore_result(asprintf(errmsg,
        "failed to pthread_create notify_thread: %s\n", strerror(err)));
  return false;
}

w_root_t *w_root_resolve_for_client_mode(const char *filename, char **errmsg)
{
  struct watchman_root *root;
  bool created = false;

  root = root_resolve(filename, true, &created, errmsg);
  if (created) {
    struct timeval start;

    /* force a walk now */
    gettimeofday(&start, NULL);
    w_root_lock(root);
    w_root_add_pending(root, root->root_path, true, start, false);
    while (root->pending) {
      w_root_process_pending(root, false);
    }
    w_root_unlock(root);
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
  watcher_ops->root_signal_threads(watcher, root);
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
  w_ht_iter_t iter;
  json_t *stopped;
  w_root_t **roots;
  uint32_t roots_count, i;

  stopped = json_array();

  pthread_mutex_lock(&root_lock);
  roots_count = w_ht_size(watched_roots);
  roots = calloc(roots_count, sizeof(*roots));

  if (!roots) {
    json_decref(stopped);
    pthread_mutex_unlock(&root_lock);
    return NULL;
  }

  i = 0;
  if (w_ht_first(watched_roots, &iter)) do {
    roots[i++] = w_ht_val_ptr(iter.value);
  } while (w_ht_next(watched_roots, &iter));

  for (i = 0; i < roots_count; i++) {
    w_root_t *root = roots[i];
    w_string_t *path = root->root_path;
    if (w_ht_del(watched_roots, w_ht_ptr_val(path))) {
      w_root_cancel(root);
      json_array_append_new(stopped, json_string_binary(path->buf, path->len));
    }
  }
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
    json_array_append_new(arr, json_string_nocheck(root->root_path->buf));
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

    w_root_lock(root);

    /* re-create the trigger configuration */
    for (j = 0; j < json_array_size(triggers); j++) {
      json_t *tobj = json_array_get(triggers, j);
      json_t *rarray;
      struct watchman_trigger_command *cmd;
      char *errmsg = NULL;

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

    json_object_set_new(obj, "path", json_string(root->root_path->buf));

    w_root_lock(root);
    triggers = w_root_trigger_list_to_json(root);
    w_root_unlock(root);
    json_object_set_new(obj, "triggers", triggers);

    json_array_append_new(watched_dirs, obj);

  } while (w_ht_next(watched_roots, &root_iter));

  pthread_mutex_unlock(&root_lock);

  json_object_set_new(state, "watched", watched_dirs);

  return result;
}

bool w_reap_children(bool block)
{
  int st;
  pid_t pid;
  int reaped = 0;

  // Reap any children so that we can release their
  // references on the root
  do {
    pid = waitpid(-1, &st, block ? 0 : WNOHANG);
    if (pid == -1) {
      break;
    }
    w_mark_dead(pid);
    reaped++;
  } while (1);

  return reaped != 0;
}

void w_root_free_watched_roots(void)
{
  w_ht_iter_t root_iter;
  int last;

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
  w_log(W_LOG_DBG, "waiting for roots to cancel and go away %d\n", last);
  for (;;) {
    int current = __sync_fetch_and_add(&live_roots, 0);
    if (current == 0) {
      break;
    }
    if (current != last) {
      w_log(W_LOG_DBG, "waiting: %d live\n", current);
      last = current;
    }
    usleep(100);
  }

  w_log(W_LOG_DBG, "all roots are gone\n");
}

/* vim:ts=2:sw=2:et:
 */
