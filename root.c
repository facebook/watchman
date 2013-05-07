/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Maps pid => root
static w_ht_t *running_kids = NULL;
static w_ht_t *watched_roots = NULL;
static int live_roots = 0;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;

/* small for testing, but should make this greater than the number of dirs we
 * have in our repos to avoid realloc */
#define HINT_NUM_DIRS 128*1024

/* We leverage the fact that our aligned pointers will never set the LSB of a
 * pointer value.  We can use the LSB to indicate whether kqueue entries are
 * dirs or files */
#define SET_DIR_BIT(dir)   ((void*)(((intptr_t)dir) | 0x1))
#define IS_DIR_BIT_SET(dir) ((((intptr_t)dir) & 0x1) == 0x1)
#define DECODE_DIR(dir)    ((void*)(((intptr_t)dir) & ~0x1))

#if defined(HAVE_KQUEUE) && !defined(O_EVTONLY)
# define O_EVTONLY O_RDONLY
#endif

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool recursive);

static void free_pending(struct watchman_pending_fs *p)
{
  w_string_delref(p->path);
  free(p);
}

static void delete_trigger(w_ht_val_t val)
{
  struct watchman_trigger_command *cmd;

  cmd = (struct watchman_trigger_command*)val;

  free(cmd->argv);
  w_string_delref(cmd->triggername);
  w_free_rules(cmd->rules);

  free(cmd);
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
  struct watchman_dir *dir;

  dir = (struct watchman_dir*)val;

  w_string_delref(dir->path);
  if (dir->files) {
    w_ht_free(dir->files);
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

static w_root_t *w_root_new(const char *path)
{
  w_root_t *root = calloc(1, sizeof(*root));
  struct watchman_dir *dir;
  pthread_mutexattr_t attr;

  assert(root != NULL);

  root->refcnt = 1;
  w_refcnt_add(&live_roots);
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&root->lock, &attr);
  pthread_mutexattr_destroy(&attr);

  pthread_cond_init(&root->cond, NULL);
  root->root_path = w_string_new(path);

  root->cursors = w_ht_new(2, &w_ht_string_funcs);
  root->suffixes = w_ht_new(2, &w_ht_string_funcs);
  root->query_cookies = w_ht_new(2, &w_ht_string_funcs);

  root->ignore_dirs = w_ht_new(2, &w_ht_string_funcs);
  // Special handling for VCS control dirs
  {
    static const char *ignores[] = {
      ".git", ".svn", ".hg"
    };
    w_string_t *name;
    w_string_t *fullname;
    uint8_t i;
    struct stat st;

    for (i = 0; i < sizeof(ignores) / sizeof(ignores[0]); i++) {
      name = w_string_new(ignores[i]);
      fullname = w_string_path_cat(root->root_path, name);
      w_ht_set(root->ignore_dirs, (w_ht_val_t)fullname, (w_ht_val_t)fullname);
      w_string_delref(fullname);

      // While we're at it, see if we can find out where to put our
      // query cookie information
      if (root->query_cookie_dir == NULL &&
          lstat(fullname->buf, &st) == 0 && S_ISDIR(st.st_mode)) {
        w_string_t *wman = w_string_new("watchman");
        w_string_t *rel;

        // {.hg,.git,.svn}/watchman
        rel = w_string_path_cat(name, wman);
        root->query_cookie_dir = w_string_path_cat(root->root_path, rel);
        w_string_delref(rel);
        w_string_delref(wman);

        // Make sure it exists
        if (mkdir(root->query_cookie_dir->buf, 0770) && errno != EEXIST) {
          w_log(W_LOG_ERR, "mkdir(%s) failed: %s\n",
              root->query_cookie_dir->buf, strerror(errno));
          w_root_delref(root);
          w_string_delref(name);
          return NULL;
        }
      }
      w_string_delref(name);
    }
  }

  root->dirname_to_dir = w_ht_new(HINT_NUM_DIRS, &dirname_hash_funcs);
  root->commands = w_ht_new(2, &trigger_hash_funcs);
  root->ticks = 1;

  // "manually" populate the initial dir, as the dir resolver will
  // try to find its parent and we don't want it to for the root
  dir = calloc(1, sizeof(*dir));
  dir->path = root->root_path;
  dir->wd = -1;
  w_string_addref(dir->path);
  w_ht_set(root->dirname_to_dir, (w_ht_val_t)dir->path, (w_ht_val_t)dir);

#if HAVE_INOTIFY_INIT
  root->wd_to_dir = w_ht_new(HINT_NUM_DIRS, NULL);
  root->infd = inotify_init();
  if (root->infd == -1) {
    w_log(W_LOG_ERR, "watch(%s): inotify_init error: %s\n",
        path, strerror(errno));
    w_root_delref(root);
    return NULL;
  }
  w_set_cloexec(root->infd);
#endif
#if HAVE_KQUEUE
  root->kq_fd = kqueue();
  if (root->kq_fd == -1) {
    w_log(W_LOG_ERR, "watch(%s): kqueue() error: %s\n",
        path, strerror(errno));
    w_root_delref(root);
    return NULL;
  }
  w_set_cloexec(root->kq_fd);
#endif
#if HAVE_PORT_CREATE
  root->port_fd = port_create();
  if (root->port_fd == -1) {
    w_log(W_LOG_ERR, "watch(%s): port_create() error: %s\n",
        path, strerror(errno));
    w_root_delref(root);
    return NULL;
  }
  w_set_cloexec(root->port_fd);
#endif

  return root;
}

void w_root_lock(w_root_t *root)
{
  int err;

  err = pthread_mutex_lock(&root->lock);
  if (err != 0) {
    w_log(W_LOG_ERR, "lock: %s\n",
        strerror(err));
  }
}

void w_root_unlock(w_root_t *root)
{
  int err;

  err = pthread_mutex_unlock(&root->lock);
  if (err != 0) {
    w_log(W_LOG_ERR, "lock: %s\n",
        strerror(err));
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
  char cookie_buf[WATCHMAN_NAME_MAX];
  char hostname[64];
  w_string_t *path_str, *cookie_base;
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

  if (gethostname(hostname, sizeof(hostname))) {
    hostname[sizeof(hostname)-1] = '\0';
  }

  /* Generate a cookie name.
   * compose host-pid-id with the cookie dir */
  w_root_lock(root);
  tick = root->ticks++;
  snprintf(cookie_buf, sizeof(cookie_buf),
      WATCHMAN_COOKIE_PREFIX "%s-%d-%" PRIu32,
      hostname, getpid(), tick);
  cookie_base = w_string_new(cookie_buf);
  if (root->query_cookie_dir) {
    path_str = w_string_path_cat(root->query_cookie_dir, cookie_base);
  } else {
    path_str = w_string_path_cat(root->root_path, cookie_base);
  }
  w_string_delref(cookie_base);
  cookie_base = NULL;

  /* insert our cookie in the map */
  w_ht_set(root->query_cookies, (w_ht_val_t)path_str, (w_ht_val_t)&cookie);

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
  w_ht_del(root->query_cookies, (w_ht_val_t)path_str);
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
  struct watchman_pending_fs *p = calloc(1, sizeof(*p));

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
  pthread_cond_signal(&root->cond);

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

bool w_root_process_pending(w_root_t *root)
{
  struct watchman_pending_fs *pending, *p;

  if (!root->pending) {
    return false;
  }

  pending = root->pending;
  root->pending = NULL;

  while (pending) {
    p = pending;
    pending = p->next;

    w_root_process_path(root, p->path, p->now, p->recursive, p->via_notify);

    free_pending(p);
  }

  return true;
}

struct watchman_dir *w_root_resolve_dir(w_root_t *root,
    w_string_t *dir_name, bool create)
{
  struct watchman_dir *dir, *parent;
  w_string_t *parent_name;

  dir = (struct watchman_dir*)w_ht_get(root->dirname_to_dir,
          (w_ht_val_t)dir_name);
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
  assert(w_ht_set(parent->dirs, (w_ht_val_t)dir_name, (w_ht_val_t)dir));
  assert(w_ht_set(root->dirname_to_dir, (w_ht_val_t)dir_name, (w_ht_val_t)dir));
//  w_log(W_LOG_DBG, "+DIR %s\n", dir_name->buf);

  return dir;
}

static void watch_file(w_root_t *root, struct watchman_file *file)
{
#if HAVE_INOTIFY_INIT
  unused_parameter(root);
  unused_parameter(file);
#else
  char buf[WATCHMAN_NAME_MAX];

#if HAVE_KQUEUE
  if (file->kq_fd != -1) {
    return;
  }
#endif

  snprintf(buf, sizeof(buf), "%.*s/%.*s",
      file->parent->path->len, file->parent->path->buf,
      file->name->len, file->name->buf);

  w_log(W_LOG_DBG, "watch_file(%s)\n", buf);

#if HAVE_KQUEUE
  {
    struct kevent k;
    file->kq_fd = open(buf, O_EVTONLY);
    if (file->kq_fd == -1) {
      w_log(W_LOG_DBG, "failed to open %s O_EVTONLY: %s\n",
          buf, strerror(errno));
      return;
    }

    memset(&k, 0, sizeof(k));
    EV_SET(&k, file->kq_fd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
        NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME|NOTE_ATTRIB,
        0, file);
    w_set_cloexec(file->kq_fd);

    if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
      perror("kevent");
      close(file->kq_fd);
      file->kq_fd = -1;
    }
  }
#endif
#if HAVE_PORT_CREATE
  file->port_file.fo_atime = file->st.st_atim;
  file->port_file.fo_mtime = file->st.st_mtim;
  file->port_file.fo_ctime = file->st.st_ctim;
  file->port_file.fo_name = buf;

  port_associate(root->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&file->port_file, WATCHMAN_PORT_EVENTS,
      (void*)file);
#endif
#endif
}

static void stop_watching_file(w_root_t *root, struct watchman_file *file)
{
#if HAVE_KQUEUE
  struct kevent k;

  if (file->kq_fd == -1) {
    return;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, file->kq_fd, EVFILT_VNODE, EV_DELETE, 0, 0, file);
  kevent(root->kq_fd, &k, 1, NULL, 0, 0);
  close(file->kq_fd);
  file->kq_fd = -1;
#elif HAVE_PORT_CREATE

  port_dissociate(root->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&file->port_file);
#else
  unused_parameter(root);
  unused_parameter(file);
#endif
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
    if (file->next) {
      file->next->prev = file->prev;
    }
    if (file->prev) {
      file->prev->next = file->next;
    }

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

static struct watchman_file *w_root_resolve_file(w_root_t *root,
    struct watchman_dir *dir, w_string_t *file_name,
    struct timeval now)
{
  struct watchman_file *file, *sufhead;
  w_string_t *suffix;

  if (dir->files) {
    file = (struct watchman_file*)w_ht_get(dir->files, (w_ht_val_t)file_name);
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
#if HAVE_KQUEUE
  file->kq_fd = -1;
#endif

  suffix = w_string_suffix(file_name);
  if (suffix) {
    sufhead = (struct watchman_file*)w_ht_get(
        root->suffixes, (w_ht_val_t)suffix);
    file->suffix_next = sufhead;
    if (sufhead) {
      sufhead->suffix_prev = file;
    }
    w_ht_replace(root->suffixes, (w_ht_val_t)suffix, (w_ht_val_t)file);
    w_string_delref(suffix);
  }

  w_ht_set(dir->files, (w_ht_val_t)file->name, (w_ht_val_t)file);
  watch_file(root, file);

  return file;
}

#ifndef HAVE_PORT_CREATE
static void schedule_recrawl(w_root_t *root)
{
  if (!root->should_recrawl) {
    w_log(W_LOG_ERR, "%.*s: something fishy: scheduling a tree recrawl\n",
        root->root_path->len, root->root_path->buf);
  }
  root->should_recrawl = true;
}
#endif

#ifdef HAVE_INOTIFY_INIT
static void invalidate_watch_descriptors(w_root_t *root,
    struct watchman_dir *dir)
{
  w_ht_iter_t i;

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    invalidate_watch_descriptors(root, child);
  } while (w_ht_next(dir->dirs, &i));

  if (dir->wd != -1) {
    w_ht_del(root->wd_to_dir, dir->wd);
    dir->wd = -1;
  }
}
#endif

static void stop_watching_dir(w_root_t *root,
    struct watchman_dir *dir, bool do_close)
{
  w_ht_iter_t i;

#ifdef HAVE_INOTIFY_INIT
  // Linux removes watches for us at the appropriate times
  return;
#endif

  w_log(W_LOG_DBG, "stop_watching_dir %.*s\n",
      dir->path->len, dir->path->buf);

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    stop_watching_dir(root, child, do_close);
  } while (w_ht_next(dir->dirs, &i));

#if HAVE_PORT_CREATE
  port_dissociate(root->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&dir->port_file);
#endif

  if (dir->wd == -1) {
    return;
  }

  /* turn off watch */
#if HAVE_INOTIFY_INIT
  if (do_close && inotify_rm_watch(root->infd, dir->wd) != 0) {
    w_log(W_LOG_ERR, "rm_watch: %d %.*s %s\n",
        dir->wd, dir->path->len, dir->path->buf,
        strerror(errno));
    schedule_recrawl(root);
  }
  w_ht_del(root->wd_to_dir, dir->wd);
  w_log(W_LOG_DBG, "removing %d -> %.*s mapping\n",
      dir->wd, dir->path->len, dir->path->buf);
#endif
#if HAVE_KQUEUE
  {
    struct kevent k;

    memset(&k, 0, sizeof(k));
    EV_SET(&k, dir->wd, EVFILT_VNODE, EV_DELETE,
        0, 0, dir);

    if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
      w_log(W_LOG_ERR, "rm_watch: %d %.*s %s\n",
          dir->wd, dir->path->len, dir->path->buf,
          strerror(errno));
      schedule_recrawl(root);
    }

    close(dir->wd);
  }
#endif
  dir->wd = -1;
}

static bool did_file_change(struct stat *saved, struct stat *fresh)
{
  /* we have to compare this way because the stat structure
   * may contain fields that vary and that don't impact our
   * understanding of the file */

#define FIELD_CHG(name) \
  if (memcmp(&saved->name, &fresh->name, sizeof(saved->name))) { \
    return true; \
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
  FIELD_CHG(st_ctime);
  FIELD_CHG(st_atime);
  FIELD_CHG(st_mtime);
  // Don't care about st_blocks
  // Don't care about st_blksize
  FIELD_CHG(WATCHMAN_ST_TIMESPEC(a));
  FIELD_CHG(WATCHMAN_ST_TIMESPEC(m));
  FIELD_CHG(WATCHMAN_ST_TIMESPEC(c));

  return false;
}

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

  if (full_path->len > sizeof(path)-1) {
    w_log(W_LOG_ERR, "path %.*s is too big\n", full_path->len, full_path->buf);
    abort();
  }

  memcpy(path, full_path->buf, full_path->len);
  path[full_path->len] = 0;

  dir_name = w_string_dirname(full_path);
  file_name = w_string_basename(full_path);
  dir = w_root_resolve_dir(root, dir_name, true);

  if (dir->files) {
    file = (struct watchman_file*)w_ht_get(dir->files, (w_ht_val_t)file_name);
  }

  if (dir->dirs) {
    dir_ent = (struct watchman_dir*)w_ht_get(dir->dirs, (w_ht_val_t)full_path);
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
      stop_watching_dir(root, dir_ent, true);
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
      file->exists = true;
      w_root_mark_file_changed(root, file, now);
    }
    memcpy(&file->st, &st, sizeof(st));
    if (S_ISDIR(st.st_mode)) {
      if (dir_ent == NULL) {
        recursive = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!w_ht_get(root->ignore_dirs, (w_ht_val_t)dir_name) ||
          // but do if we're looking at the cookie dir
          (root->query_cookie_dir &&
          w_string_equal(full_path, root->query_cookie_dir))) {
#ifndef HAVE_INOTIFY_INIT
        /* On non-Linux systems, we always need to crawl, but may not
         * need to be fully recursive */
        crawler(root, full_path, now, recursive);
#else
        /* On Linux systems we get told about change on the child, so we only
         * need to crawl if we've never seen the dir before */

        if (recursive) {
          crawler(root, full_path, now, recursive);
        }
#endif
      }
    }
  }

  w_string_delref(dir_name);
  w_string_delref(file_name);
}


void w_root_process_path(w_root_t *root, w_string_t *full_path,
    struct timeval now, bool recursive, bool via_notify)
{
  if (w_string_is_cookie(full_path)) {
    struct watchman_query_cookie *cookie;

    cookie = (void*)w_ht_get(root->query_cookies, (w_ht_val_t)full_path);
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
    struct watchman_file *file = (struct watchman_file*)i.value;

    if (file->exists) {
      w_log(W_LOG_DBG, "mark_deleted: %.*s/%.*s\n",
          dir->path->len, dir->path->buf,
          file->name->len, file->name->buf);
      file->exists = false;
      w_root_mark_file_changed(root, file, now);
    }

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    w_root_mark_deleted(root, child, now, true);
  } while (w_ht_next(dir->dirs, &i));
}

#if HAVE_INOTIFY_INIT
struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd)
{
  return (struct watchman_dir*)w_ht_get(root->wd_to_dir, wd);
}
#endif

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool recursive)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  DIR *osdir;
  struct dirent *dirent;
  w_ht_iter_t i;
  char path[WATCHMAN_NAME_MAX];
  int err;

  dir = w_root_resolve_dir(root, dir_name, true);

  memcpy(path, dir_name->buf, dir_name->len);
  path[dir_name->len] = 0;

  w_log(W_LOG_DBG, "opendir(%s) recursive=%s\n",
      path, recursive ? "true" : "false");
  osdir = opendir(path);
  if (!osdir) {
    err = errno;
    if (err == ENOENT || err == ENOTDIR) {
      if (w_string_equal(dir_name, root->root_path)) {
        w_log(W_LOG_ERR,
            "opendir(%s) -> %s. Root was deleted; cancelling watch\n",
            path, strerror(err));
        w_root_cancel(root);
        return;
      }

      w_log(W_LOG_DBG, "opendir(%s) -> %s so stopping watch\n",
          path, strerror(err));
      stop_watching_dir(root, dir, true);
      w_root_mark_deleted(root, dir, now, true);
    }
    return;
  }

  /* make sure we're watching this guy */
  if (dir->wd == -1) {
    w_log(W_LOG_DBG, "watch_dir(%s)\n", path);
#if HAVE_INOTIFY_INIT
    dir->wd = inotify_add_watch(root->infd, path,
        WATCHMAN_INOTIFY_MASK);
    /* record mapping */
    if (dir->wd != -1) {
      w_ht_replace(root->wd_to_dir, dir->wd, (w_ht_val_t)dir);
      w_log(W_LOG_DBG, "adding %d -> %s mapping\n", dir->wd, path);
    }
#endif
#if HAVE_KQUEUE
    dir->wd = open(path, O_EVTONLY);
    if (dir->wd != -1) {
      struct kevent k;

      memset(&k, 0, sizeof(k));
      EV_SET(&k, dir->wd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
        NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME,
        0,
        SET_DIR_BIT(dir));
      w_set_cloexec(dir->wd);

      if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
        w_log(W_LOG_DBG, "kevent EV_ADD dir %s failed: %s",
            path, strerror(errno));
        close(dir->wd);
        dir->wd = -1;
      }
    } else {
      w_log(W_LOG_DBG, "couldn't open dir %s for watching: %s\n",
          path, strerror(errno));
    }
#endif
#if HAVE_PORT_CREATE
    {
      struct stat st;

      lstat(path, &st);
      dir->port_file.fo_atime = st.st_atim;
      dir->port_file.fo_mtime = st.st_mtim;
      dir->port_file.fo_ctime = st.st_ctim;
      dir->port_file.fo_name = (char*)dir->path->buf;

      errno = 0;
      port_associate(root->port_fd, PORT_SOURCE_FILE,
          (uintptr_t)&dir->port_file, WATCHMAN_PORT_EVENTS,
          SET_DIR_BIT(dir));
      w_log(W_LOG_ERR, "port_associate %s %s\n",
        dir->port_file.fo_name, strerror(errno));
    }
#endif
  }

  /* flag for delete detection */
  if (w_ht_first(dir->files, &i)) do {
    file = (struct watchman_file*)i.value;
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
      file = (struct watchman_file*)w_ht_get(dir->files, (w_ht_val_t)name);
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
    file = (struct watchman_file*)i.value;
    if (file->exists && (file->maybe_deleted ||
          (S_ISDIR(file->st.st_mode) && recursive))) {
      w_root_add_pending_rel(root, dir, file->name->buf,
          recursive, now, false);
    }
  } while (w_ht_next(dir->files, &i));
}

static void spawn_command(w_root_t *root,
    struct watchman_trigger_command *cmd,
    uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  char **argv = NULL;
  uint32_t argc = 0;
  uint32_t i = 0, j;
  long len, argmax;
  int ret;
  int json_fd = -1;
  char json_file_name[WATCHMAN_NAME_MAX];
  json_t *file_list = NULL;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  w_jbuffer_t buffer;
  sigset_t mask;

  file_list = w_match_results_to_json(num_matches, matches);
  if (!file_list) {
    w_log(W_LOG_ERR, "unable to render matches to json: %s\n",
        strerror(errno));
    goto out;
  }

  /* prepare the json input stream for the child process */
  snprintf(json_file_name, sizeof(json_file_name), "%s/wmanXXXXXX",
      watchman_tmp_dir);
  json_fd = mkstemp(json_file_name);
  if (json_fd == -1) {
    /* failed to make the stream :-/ */

    w_log(W_LOG_ERR, "unable to create a temporary file: %s\n",
        strerror(errno));

    goto out;
  }

  /* unlink the file, we don't need it in the filesystem;
   * we'll pass the fd on to the child as stdin */
  unlink(json_file_name);
  if (!w_json_buffer_init(&buffer)) {
    w_log(W_LOG_ERR, "failed to init json buffer\n");
    goto out;
  }
  w_json_buffer_write(&buffer, json_fd, file_list, 0);
  w_json_buffer_free(&buffer);
  lseek(json_fd, 0, SEEK_SET);

  /* if we make the command line too long, things blow up.
   * We use a little less than the max in case the shell
   * needs some of that space */
  argmax = sysconf(_SC_ARG_MAX) - 24;

  argc = cmd->argc + num_matches;
  argv = calloc(argc + 1, sizeof(char*));
  if (!argv) {
    w_log(W_LOG_ERR, "out of memory\n");
    goto out;
  }

  /* copy in the base command */
  len = 0;
  for (i = 0; i < cmd->argc; i++) {
    argv[i] = cmd->argv[i];
    len += 1 + strlen(argv[i]);
  }

  /* now fill out the file name args.
   * We stop adding when the command line is too big.
   */
  for (j = 0; j < num_matches; j++) {
    w_string_t *relname = matches[j].relname;

    if (relname->len + 1 + len >= argmax) {
      break;
    }
    argv[i++] = w_string_dup_buf(relname);
    len += relname->len + 1;
  }
  argv[i] = NULL;

  posix_spawnattr_init(&attr);
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  posix_spawnattr_setflags(&attr,
      POSIX_SPAWN_SETSIGMASK|
      POSIX_SPAWN_SETPGROUP);

  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, json_fd, STDIN_FILENO);
  // TODO: std{in,out,err} redirection by default?

  ignore_result(chdir(root->root_path->buf));

  pthread_mutex_lock(&spawn_lock);
  cmd->dispatch_tick = root->ticks;
  ret = posix_spawnp(&cmd->current_proc,
      argv[0], &actions,
      &attr, argv, environ);
  if (ret == 0) {
    w_ht_set(running_kids, cmd->current_proc,
        (w_ht_val_t)root);
  }
  pthread_mutex_unlock(&spawn_lock);

  w_log(W_LOG_DBG, "posix_spawnp: argc=%d\n", argc);
  for (i = 0; i < argc; i++) {
    w_log(W_LOG_DBG, "  [%d] %s\n", i, argv[i]);
  }
  w_log(W_LOG_DBG, "pid=%d ret=%d\n", cmd->current_proc, ret);

  ignore_result(chdir("/"));

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);

out:
  w_match_results_free(num_matches, matches);

  if (argv) {
    for (i = cmd->argc; i < argc; i++) {
      free(argv[i]);
    }
    free(argv);
  }

  if (json_fd != -1) {
    close(json_fd);
  }

  if (file_list) {
    json_decref(file_list);
  }
}

void w_mark_dead(pid_t pid)
{
  w_root_t *root;
  w_ht_iter_t iter;

  pthread_mutex_lock(&spawn_lock);
  root = (w_root_t*)w_ht_get(running_kids, pid);
  if (!root) {
    pthread_mutex_unlock(&spawn_lock);
    return;
  }
  w_ht_del(running_kids, pid);
  pthread_mutex_unlock(&spawn_lock);

  /* now walk the cmds and try to find our match */
  w_root_lock(root);

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd;
    struct watchman_file *f, *oldest = NULL;
    struct watchman_rule_match *results = NULL;
    uint32_t matches;
    struct w_clockspec_query since;

    cmd = (struct watchman_trigger_command*)iter.value;
    if (cmd->current_proc != pid) {
      continue;
    }

    /* first mark the process as dead */
    cmd->current_proc = 0;

    since.is_timestamp = false;
    since.ticks = cmd->dispatch_tick;

    /* now we need to figure out if more updates came
     * in while we were running */
    for (f = root->latest_file;
        f && f->otime.ticks > cmd->dispatch_tick;
        f = f->next) {
      oldest = f;
    }

    matches = w_rules_match(root, oldest, &results, cmd->rules, &since);
    if (matches > 0) {
      spawn_command(root, cmd, matches, results);
    }

    break;
  } while (w_ht_next(root->commands, &iter));

  w_root_unlock(root);
}

static inline struct watchman_file *find_oldest_with_tick(
    w_root_t *root, uint32_t tick)
{
  struct watchman_file *f, *oldest = NULL;

  for (f = root->latest_file;
      f && f->otime.ticks > tick;
      f = f->next) {

    oldest = f;
  }

  return oldest;
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
    struct watchman_client *client = (void*)iter.value;
    w_ht_iter_t citer;

    w_log(W_LOG_DBG, "client=%p fd=%d\n", client, client->fd);

    if (w_ht_first(client->subscriptions, &citer)) do {
      struct watchman_client_subscription *sub = (void*)citer.value;

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
  file = (struct watchman_file*)w_ht_get(dir->files, (w_ht_val_t)file_name);
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
  struct watchman_file *oldest = NULL;
  struct watchman_rule_match *results = NULL;
  uint32_t matches;
  w_ht_iter_t iter;
  struct w_clockspec_query since;

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

  oldest = find_oldest_with_tick(root, root->last_trigger_tick);

  since.is_timestamp = false;
  since.ticks = root->last_trigger_tick;

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd;

    cmd = (struct watchman_trigger_command*)iter.value;
    if (cmd->current_proc) {
      // Don't spawn if there's one already running
      continue;
    }
    matches = w_rules_match(root, oldest, &results, cmd->rules, &since);
    if (matches > 0) {
      spawn_command(root, cmd, matches, results);
    }

  } while (w_ht_next(root->commands, &iter));

  root->last_trigger_tick = root->pending_trigger_tick;
}

static void handle_should_recrawl(w_root_t *root)
{
  if (root->should_recrawl) {
    struct timeval now;

    gettimeofday(&now, NULL);
    root->should_recrawl = false;
    w_root_add_pending(root, root->root_path, true, now, false);
  }
}

static void *stat_thread(void *arg)
{
  w_root_t *root = arg;
  struct timeval start, end, now, target;
  struct timeval settle, recrawl;

  settle.tv_sec = trigger_settle / 1000;
  settle.tv_usec = (trigger_settle - (settle.tv_sec * 1000)) * 1000;
  recrawl.tv_sec = recrawl_period / 1000;
  recrawl.tv_usec = (recrawl_period - (recrawl.tv_sec * 1000)) * 1000;

  /* first order of business is to find all the files under our root */
  gettimeofday(&start, NULL);
  w_root_lock(root);
  w_root_add_pending(root, root->root_path, false, start, false);
  w_root_unlock(root);

  /* now we just sit and wait for things to land in our pending list */
  for (;;) {
    int err;
    struct timespec ts;

    w_root_lock(root);
    if (root->cancelled) {
      w_root_unlock(root);
      break;
    }
    handle_should_recrawl(root);

    if (!root->pending) {

      // Throttle our trigger rate
      gettimeofday(&now, NULL);
      if (root->latest_file) {
        w_timeval_add(root->latest_file->otime.tv, settle, &target);
        if (w_timeval_compare(now, target) < 0) {
          // Still have a bit of time to wait

          w_timeval_to_timespec(target, &ts);
          err = pthread_cond_timedwait(&root->cond, &root->lock, &ts);

          // The condwait unlocks root; we may have picked up some
          // more pending items
          if (err != ETIMEDOUT || root->pending) {
            // We have more pending items to collect
            goto have_pending;
          }
        }
      }

      if (!root->done_initial) {
        gettimeofday(&end, NULL);
        w_log(W_LOG_DBG, "%s scanned in %.2f seconds\n",
            root->root_path->buf,
            w_timeval_diff(start, end));
        root->done_initial = true;
      }

      process_triggers(root);
      process_subscriptions(root);

      /* We may have released the lock while processing items;
       * we may have more work to do; make sure we don't fall
       * asleep on the job */
      if (root->pending) {
        goto have_pending;
      }

      if (recrawl_period) {
        gettimeofday(&now, NULL);
        w_timeval_add(now, recrawl, &target);
        w_timeval_to_timespec(target, &ts);
        err = pthread_cond_timedwait(&root->cond, &root->lock, &ts);
        if (err != 0 && err != ETIMEDOUT) {
          w_log(W_LOG_ERR, "pthread_cond_wait: %s\n",
              strerror(err));
          w_root_lock(root);
        }
        if (err == ETIMEDOUT && !root->pending) {
          // periodically scan the tree and fix it up
          root->should_recrawl = true;
        }
      } else {
        pthread_cond_wait(&root->cond, &root->lock);
      }
    }
have_pending:
    if (root->cancelled) {
      w_root_unlock(root);
      break;
    }
    w_root_process_pending(root);
    w_root_unlock(root);
  }
  w_log(W_LOG_DBG, "stat_thread: out of loop %s\n",
      root->root_path->buf);
  w_root_delref(root);

  return 0;
}

#if HAVE_KQUEUE

static int consume_kqueue(w_root_t *root, w_ht_t *batch,
    bool timeout)
{
  struct kevent k[32];
  int n;
  int i;
  struct timespec ts = { 0, 200000 };

  errno = 0;

  w_log(W_LOG_DBG, "kqueue(%s) timeout=%d\n",
      root->root_path->buf, timeout);
  n = kevent(root->kq_fd, NULL, 0,
        k, sizeof(k) / sizeof(k[0]),
        timeout ? &ts : NULL);
  w_log(W_LOG_DBG, "consume_kqueue: %s timeout=%d n=%d err=%s\n",
      root->root_path->buf, timeout, n, strerror(errno));
  if (root->cancelled) {
    return 0;
  }

  for (i = 0; n > 0 && i < n; i++) {
    intptr_t p = (intptr_t)k[i].udata;

    if (IS_DIR_BIT_SET(p)) {
      struct watchman_dir *dir = DECODE_DIR(p);

      w_log(W_LOG_DBG, " KQ dir %s\n", dir->path->buf);
      w_ht_set(batch, (w_ht_val_t)dir->path, (w_ht_val_t)p);
    } else {
      struct watchman_file *file = (void*)p;
      w_string_t *name;

      name = w_string_path_cat(file->parent->path, file->name);
      w_ht_set(batch, (w_ht_val_t)name, (w_ht_val_t)p);
      w_log(W_LOG_DBG, " KQ file %s\n", name->buf);
      w_string_delref(name);
    }
  }

  return n;
}

static void kqueue_thread(w_root_t *root)
{
  w_ht_t *batch = NULL;

  for (;;) {
    struct timeval now;
    int n;

    if (!batch) {
      batch = w_ht_new(2, &w_ht_string_funcs);
    }

    w_log(W_LOG_DBG, "Blocking until we get kqueue activity %s\n",
        root->root_path->buf);

    /* get a batch of events, and allow a little bit of
     * time for them to arrive (I've seen several events
     * for the same item delivered one at a time */
    n = consume_kqueue(root, batch, false);
    while (n > 0) {
      n = consume_kqueue(root, batch, true);
    }
    if (root->cancelled) {
      break;
    }

    w_log(W_LOG_DBG, "Have %d events in %s\n",
        w_ht_size(batch), root->root_path->buf);

    if (w_ht_size(batch)) {
      w_ht_iter_t iter;

      w_root_lock(root);
      root->ticks++;
      gettimeofday(&now, NULL);
      if (w_ht_first(batch, &iter)) do {
        w_string_t *name = (w_string_t*)iter.key;
        intptr_t p = iter.value;

        w_log(W_LOG_DBG, "kq -> %s\n", name->buf);
        /* we only want to set the via_notify flag to true if we're
         * absolutely sure that something was changed.
         * Since we get notified on dirs when their contents change,
         * we can't be sure precisely what changed (we'll compare stat results
         * to find out).  For files, we're absolute sure that they changed.
         * Therefore, we set via_notify to true if this was a file notification,
         * and false otherwise */
        w_root_add_pending(root, name, false, now, !IS_DIR_BIT_SET(p));

      } while (w_ht_next(batch, &iter));

      w_root_unlock(root);

      w_ht_free(batch);
      batch = NULL;
    }
  }

  if (batch) {
    w_ht_free(batch);
  }
}

#endif

#if HAVE_PORT_CREATE
static void portfs_thread(w_root_t *root)
{
  for (;;) {
    port_event_t events[128];
    uint_t i, n;
    struct timeval now;

    if (root->cancelled) {
      break;
    }

    n = 1;
    if (port_getn(root->port_fd, events,
          sizeof(events) / sizeof(events[0]), &n, NULL)) {
      if (errno == EINTR) {
        continue;
      }
      w_log(W_LOG_ERR, "port_getn: %s\n",
          strerror(errno));
      abort();
    }

    w_log(W_LOG_ERR, "port_getn: n=%u\n", n);

    if (n == 0) {
      continue;
    }

    w_root_lock(root);
    root->ticks++;
    gettimeofday(&now, NULL);

    for (i = 0; i < n; i++) {
      if (IS_DIR_BIT_SET(events[i].portev_user)) {
        struct watchman_dir *dir = DECODE_DIR(events[i].portev_user);

        w_root_add_pending(root, dir->path, true, now, true);

      } else {
        struct watchman_file *file = events[i].portev_user;
        w_string_t *path;

        path = w_string_path_cat(file->parent->path, file->name);
        w_root_add_pending(root, path, true, now, true);

        file->port_file.fo_name = (char*)path->buf;
        w_string_delref(path);
      }
    }

    w_root_unlock(root);
  }
}
#endif

#if HAVE_INOTIFY_INIT
// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work to the
// stat_thread above.
static void inotify_thread(w_root_t *root)
{
  /* now we can settle into the notification stuff */
  while (!root->cancelled) {
    struct inotify_event *ine;
    char *iptr;
    int n;
    struct timeval now;
    struct watchman_dir *dir;
    struct watchman_file *file = NULL;
    w_string_t *name;
    struct pollfd pfd;

    pfd.fd = root->infd;
    pfd.events = POLLIN;

    n = poll(&pfd, 1, 100);

    if (root->cancelled) {
      break;
    }

    if (n != 1) {
      continue;
    }

    n = read(root->infd, &root->ibuf, sizeof(root->ibuf));
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      w_log(W_LOG_ERR, "read(%d, %lu): error %s\n",
          root->infd, sizeof(root->ibuf), strerror(errno));
      abort();
    }

    w_log(W_LOG_DBG, "inotify read: returned %d.\n", n);

    w_root_lock(root);
    root->ticks++;
    gettimeofday(&now, NULL);

    for (iptr = root->ibuf; iptr < root->ibuf + n;
          iptr = iptr + sizeof(*ine) + ine->len) {
      ine = (struct inotify_event*)iptr;

      w_log(W_LOG_DBG, "notify: wd=%d mask=%x %s\n", ine->wd, ine->mask,
          ine->len > 0 ? ine->name : "");


      if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
        /* we missed something, will need to re-crawl */

        w_log(W_LOG_ERR, "inotify: IN_Q_OVERFLOW, re-crawling %.*s\n",
            root->root_path->len,
            root->root_path->buf);
        schedule_recrawl(root);
      } else if (ine->wd != -1) {
        char buf[WATCHMAN_NAME_MAX];

        dir = w_root_resolve_dir_by_wd(root, ine->wd);
        if (dir) {
          // The hint is that it has gone away, so remove our wd mapping here.
          // We'll queue up a stat of that dir anyway so that we really know
          // the state of that path, but that has to happen /after/ we've
          // removed this watch.
          if ((ine->mask & IN_IGNORED) != 0) {
            w_log(W_LOG_DBG, "mask=%x: remove watch %d %.*s\n", ine->mask,
                dir->wd, dir->path->len, dir->path->buf);
            if (dir->wd != -1) {
              w_ht_del(root->wd_to_dir, dir->wd);
              dir->wd = -1;
            }
          }

          // We need to ensure that the watch descriptor associations from
          // the old location are no longer valid so that when we crawl
          // the destination location we'll update the entries
          if (ine->len > 0 && (ine->mask & IN_MOVED_FROM)) {
            name = w_string_new(ine->name);
            file = w_root_resolve_file(root, dir, name, now);
            if (!file) {
              w_log(W_LOG_ERR,
                  "looking for file %.*s but it is missing in %.*s\n",
                  ine->len, ine->name, dir->path->len, dir->path->buf);
              schedule_recrawl(root);
              w_string_delref(name);
              continue;
            }

            // The file no longer exists in its old location
            file->exists = false;
            w_root_mark_file_changed(root, file, now);
            w_string_delref(name);

            // Was there a dir here too?
            snprintf(buf, sizeof(buf), "%.*s/%s",
                dir->path->len, dir->path->buf,
                ine->name);
            name = w_string_new(buf);

            dir = w_root_resolve_dir(root, name, false);
            if (dir) {
              // Ensure that the old tree is not associated
              invalidate_watch_descriptors(root, dir);
              // and marked deleted
              w_root_mark_deleted(root, dir, now, true);
            }
            w_string_delref(name);
            continue;
          }


          if (ine->len > 0) {
            snprintf(buf, sizeof(buf), "%.*s/%s",
                dir->path->len, dir->path->buf,
                ine->name);
            name = w_string_new(buf);
          } else {
            name = dir->path;
            w_string_addref(name);
          }

          if ((ine->mask & (IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF)) != 0) {
            w_string_t *pname;

            if (w_string_equal(root->root_path, name)) {
              w_log(W_LOG_ERR,
                  "root dir %s has been (re)moved, canceling watch\n",
                  root->root_path->buf);
              w_root_cancel(root);
              break;
            }

            // We need to examine the parent and crawl down
            pname = w_string_dirname(name);
            w_log(W_LOG_DBG, "mask=%x, focus on parent: %.*s\n",
                ine->mask, pname->len, pname->buf);
            w_string_delref(name);
            name = pname;
          }

          w_log(W_LOG_DBG, "add_pending for inotify mask=%x %.*s\n",
              ine->mask, name->len, name->buf);
          w_root_add_pending(root, name, true, now, true);

          w_string_delref(name);

        } else if ((ine->mask & (IN_MOVE_SELF|IN_IGNORED)) == 0) {
          // If we can't resolve the dir, and this isn't notification
          // that it has gone away, then we want to recrawl to fix
          // up our state.
          w_log(W_LOG_ERR, "wanted dir %d, but not found %s\n",
              ine->wd, ine->name);
          schedule_recrawl(root);
        }
      }
    }

    handle_should_recrawl(root);
    w_root_unlock(root);
  }

}
#endif

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

static void delete_file(struct watchman_file *file)
{
  w_string_delref(file->name);
  free(file);
}

void w_root_delref(w_root_t *root)
{
  struct watchman_file *file;

  if (!w_refcnt_del(&root->refcnt)) return;

  w_log(W_LOG_DBG, "root: final ref on %s\n",
      root->root_path->buf);

  while (root->pending) {
    struct watchman_pending_fs *p;

    p = root->pending;
    root->pending = p->next;
    w_string_delref(p->path);
    free(p);
  }

  pthread_mutex_destroy(&root->lock);
  w_string_delref(root->root_path);
  pthread_cond_destroy(&root->cond);
  w_ht_free(root->cursors);
  w_ht_free(root->suffixes);
  w_ht_free(root->ignore_dirs);
  w_ht_free(root->dirname_to_dir);
  w_ht_free(root->commands);
  w_ht_free(root->query_cookies);

#ifdef HAVE_INOTIFY_INIT
  close(root->infd);
  w_ht_free(root->wd_to_dir);
#endif
#ifdef HAVE_KQUEUE
  close(root->kq_fd);
#endif
#ifdef HAVE_PORT_CREATE
  close(root->port_fd);
#endif

  while (root->latest_file) {
    file = root->latest_file;
    root->latest_file = file->next;
    delete_file(file);
  }

  if (root->query_cookie_dir) {
    w_string_delref(root->query_cookie_dir);
  }

  free(root);
  w_refcnt_del(&live_roots);
}

static w_ht_val_t root_copy_val(w_ht_val_t val)
{
  w_root_t *root = (w_root_t*)val;

  w_root_addref(root);

  return val;
}

static void root_del_val(w_ht_val_t val)
{
  w_root_t *root = (w_root_t*)val;

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

static w_root_t *root_resolve(const char *filename, bool auto_watch,
    bool *created)
{
  struct watchman_root *root = NULL;
  w_ht_val_t root_val;
  char *watch_path;
  w_string_t *root_str;

  *created = false;
  watch_path = w_realpath(filename);

  if (!watch_path) {
    w_log(W_LOG_ERR, "resolve_root: %s: %s\n", filename, strerror(errno));
    return NULL;
  }

  root_str = w_string_new(watch_path);
  pthread_mutex_lock(&root_lock);
  if (!watched_roots) {
    watched_roots = w_ht_new(4, &root_funcs);
  }
  // This will addref if it returns root
  if (w_ht_lookup(watched_roots, (w_ht_val_t)root_str, &root_val, true)) {
    root = (w_root_t*)root_val;
  }
  pthread_mutex_unlock(&root_lock);
  w_string_delref(root_str);

  if (root || !auto_watch) {
    free(watch_path);
    // caller owns a ref
    return root;
  }

  w_log(W_LOG_DBG, "Want to watch %s -> %s\n", filename, watch_path);

  // created with 1 ref
  root = w_root_new(watch_path);
  free(watch_path);

  if (!root) {
    return NULL;
  }

  *created = true;

  pthread_mutex_lock(&root_lock);
  // adds 1 ref
  w_ht_set(watched_roots, (w_ht_val_t)root->root_path, (w_ht_val_t)root);
  if (!running_kids) {
    running_kids = w_ht_new(2, NULL);
  }
  pthread_mutex_unlock(&root_lock);

  // caller owns 1 ref
  return root;
}

static void *run_notify_thread(void *arg)
{
  w_root_t *root = arg;

#if HAVE_INOTIFY_INIT
  inotify_thread(root);
#elif HAVE_KQUEUE
  kqueue_thread(root);
#elif HAVE_PORT_CREATE
  portfs_thread(root);
#else
# error I dont support this system
#endif

  w_log(W_LOG_DBG, "notify_thread: out of loop %s\n",
      root->root_path->buf);

  /* we'll remove it from watched roots if it isn't
   * already out of there */
  pthread_mutex_lock(&root_lock);
  w_ht_del(watched_roots, (w_ht_val_t)root->root_path);
  pthread_mutex_unlock(&root_lock);

  w_root_delref(root);
  return 0;
}

static bool root_start(w_root_t *root)
{
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  w_root_addref(root);
  pthread_create(&root->notify_thread, &attr, run_notify_thread, root);

  w_root_addref(root);
  pthread_create(&root->stat_thread, &attr, stat_thread, root);

  pthread_attr_destroy(&attr);

  return root;
}

w_root_t *w_root_resolve_for_client_mode(const char *filename)
{
  struct watchman_root *root;
  bool created = false;

  root = root_resolve(filename, true, &created);
  if (created) {
    struct timeval start;

    /* force a walk now */
    gettimeofday(&start, NULL);
    w_root_lock(root);
    w_root_add_pending(root, root->root_path, true, start, false);
    while (root->pending) {
      w_root_process_pending(root);
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
  if (!pthread_equal(root->stat_thread, pthread_self())) {
    pthread_cond_signal(&root->cond);
    pthread_kill(root->stat_thread, SIGUSR1);
  }
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
  bool stopped = false;

  pthread_mutex_lock(&root_lock);
  stopped = w_ht_del(watched_roots, (w_ht_val_t)root->root_path);
  pthread_mutex_unlock(&root_lock);

  if (stopped) {
    w_root_cancel(root);
    w_state_save();
  }
  signal_root_threads(root);

  return stopped;
}

w_root_t *w_root_resolve(const char *filename, bool auto_watch)
{
  struct watchman_root *root;
  bool created = false;

  root = root_resolve(filename, auto_watch, &created);
  if (created) {
    root_start(root);
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
    struct watchman_trigger_command *cmd = (void*)iter.value;
    struct watchman_rule *rule;
    json_t *obj = json_object();
    json_t *args = json_array();
    json_t *rules = json_array();
    uint32_t i;

    json_object_set_new(obj, "name", json_string(cmd->triggername->buf));
    for (i = 0; i < cmd->argc; i++) {
      json_array_append_new(args, json_string(cmd->argv[i]));
    }
    json_object_set_new(obj, "command", args);

    for (rule = cmd->rules; rule; rule = rule->next) {
      json_t *robj = json_object();

      json_object_set_new(robj, "pattern", json_string(rule->pattern));
      json_object_set_new(robj, "include", json_boolean(rule->include));
      json_object_set_new(robj, "negated", json_boolean(rule->negated));

      json_array_append_new(rules, robj);
    }
    json_object_set_new(obj, "rules", rules);

    json_array_append_new(arr, obj);

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
    w_root_t *root = (void*)iter.value;
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

    triggers = json_object_get(obj, "triggers");
    filename = json_string_value(json_object_get(obj, "path"));
    root = root_resolve(filename, true, &created);

    if (!root) {
      continue;
    }

    w_root_lock(root);

    /* re-create the trigger configuration */
    for (j = 0; j < json_array_size(triggers); j++) {
      json_t *tobj = json_array_get(triggers, j);
      json_t *cmdarray, *rarray;
      json_t *robj;
      struct watchman_trigger_command *cmd;
      struct watchman_rule *rule, *prior = NULL;
      size_t r;

      cmdarray = json_object_get(tobj, "command");
      cmd = calloc(1, sizeof(*cmd));
      cmd->argc = json_array_size(cmdarray);
      cmd->argv = w_argv_copy_from_json(cmdarray, 0);
      cmd->triggername = w_string_new(json_string_value(
                          json_object_get(tobj, "name")));

      rarray = json_object_get(tobj, "rules");
      for (r = 0; r < json_array_size(rarray); r++) {
        int include = 1, negate = 0;
        const char *pattern = NULL;

        robj = json_array_get(rarray, r);

        if (json_unpack(robj, "{s:s, s:b, s:b}",
              "pattern", &pattern,
              "include", &include,
              "negated", &negate) == 0) {
          rule = calloc(1, sizeof(*rule));
          rule->include = include;
          rule->negated = negate;
          rule->pattern = strdup(pattern);
          rule->flags = FNM_PERIOD;

          if (!prior) {
            cmd->rules = rule;
          } else {
            prior->next = rule;
          }
          prior = rule;
        }
      }

      w_ht_replace(root->commands, (w_ht_val_t)cmd->triggername,
          (w_ht_val_t)cmd);
    }

    w_root_unlock(root);

    if (created) {
      root_start(root);
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

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    w_root_t *root = (void*)root_iter.value;
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

void w_root_free_watched_roots(void)
{
  w_ht_iter_t root_iter;
  int last;

  pthread_mutex_lock(&root_lock);
  if (w_ht_first(watched_roots, &root_iter)) do {
    w_root_t *root = (void*)root_iter.value;
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

