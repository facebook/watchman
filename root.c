/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"
#include <spawn.h>
// Not explicitly exported on Darwin, so we get to define it.
extern char **environ;

static w_ht_t *watched_roots = NULL;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

/* small for testing, but should make this greater than the number of dirs we
 * have in our repos to avoid realloc */
#define HINT_NUM_DIRS 16*1024

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool confident);

static void free_pending(struct watchman_pending_fs *p)
{
  w_string_delref(p->path);
  free(p);
}

w_root_t *w_root_new(const char *path)
{
  w_root_t *root = calloc(1, sizeof(*root));
  struct watchman_dir *dir;
  pthread_mutexattr_t attr;

  assert(root != NULL);

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&root->lock, NULL);
  pthread_mutexattr_destroy(&attr);

  pthread_cond_init(&root->cond, NULL);
  root->root_path = w_string_new(path);

  root->cursors = w_ht_new(2, &w_ht_string_funcs);

#if HAVE_INOTIFY_INIT
  root->infd = inotify_init();
  w_set_cloexec(root->infd);
  root->wd_to_dir = w_ht_new(HINT_NUM_DIRS, NULL);
#endif
#if HAVE_KQUEUE
  root->kq_fd = kqueue();
  w_set_cloexec(root->kq_fd);
#endif
  root->dirname_to_dir = w_ht_new(HINT_NUM_DIRS, &w_ht_string_funcs);
  root->commands = w_ht_new(2, NULL);
  root->ticks = 1;

  // "manually" populate the initial dir, as the dir resolver will
  // try to find its parent and we don't want it to for the root
  dir = calloc(1, sizeof(*dir));
  dir->path = root->root_path;
  dir->wd = -1;
  w_string_addref(dir->path);
  w_ht_set(root->dirname_to_dir, (w_ht_val_t)dir->path, (w_ht_val_t)dir);

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

bool w_root_add_pending(w_root_t *root, w_string_t *path,
    bool confident, struct timeval now, bool via_notify)
{
  struct watchman_pending_fs *p = calloc(1, sizeof(*p));

  if (!p) {
    return false;
  }

  p->confident = confident;
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
    const char *name, bool confident,
    struct timeval now, bool via_notify)
{
  char path[WATCHMAN_NAME_MAX];
  w_string_t *path_str;
  bool res;

  snprintf(path, sizeof(path), "%.*s/%s", dir->path->len, dir->path->buf, name);
  path_str = w_string_new(path);

  res = w_root_add_pending(root, path_str, confident, now, via_notify);

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

    w_root_process_path(root, p->path, p->now, p->confident);

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
#if HAVE_KQUEUE
  struct kevent k;
  char buf[WATCHMAN_NAME_MAX];

  if (file->kq_fd != -1) {
    return;
  }

  snprintf(buf, sizeof(buf), "%.*s/%.*s",
      file->parent->path->len, file->parent->path->buf,
      file->name->len, file->name->buf);

  file->kq_fd = open(buf, O_EVTONLY);
  if (file->kq_fd == -1) {
    w_log(W_LOG_DBG, "failed to open %s O_EVTONLY: %s\n",
        buf, strerror(errno));
    return;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, file->kq_fd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
    NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME|NOTE_NONE|NOTE_ATTRIB,
    0, file);
  w_set_cloexec(file->kq_fd);

  if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
    perror("kevent");
    close(file->kq_fd);
    file->kq_fd = -1;
  }
#else
  unused_parameter(root);
  unused_parameter(file);
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
#else
  unused_parameter(root);
  unused_parameter(file);
#endif
}

void w_root_mark_file_changed(w_root_t *root, struct watchman_file *file,
    struct timeval now, bool confident)
{
  if (file->exists) {
    watch_file(root, file);
  } else {
    stop_watching_file(root, file);
  }

  file->confident = confident;
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

}

struct watchman_file *w_root_resolve_file(w_root_t *root,
    struct watchman_dir *dir, w_string_t *file_name)
{
  struct watchman_file *file;

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
#if HAVE_KQUEUE
  file->kq_fd = -1;
#endif

  w_ht_set(dir->files, (w_ht_val_t)file->name, (w_ht_val_t)file);
  watch_file(root, file);

  return file;
}

static void stop_watching_dir(w_root_t *root,
    struct watchman_dir *dir)
{
  w_ht_iter_t i;

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    stop_watching_dir(root, child);
  } while (w_ht_next(dir->dirs, &i));

  if (dir->wd == -1) {
    return;
  }

  /* turn off watch */
#if HAVE_INOTIFY_INIT
  if (inotify_rm_watch(root->infd, dir->wd) != 0) {
    w_log(W_LOG_ERR, "rm_watch: %d %.*s %s\n",
        dir->wd, dir->path->len, dir->path->buf,
        strerror(errno));
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
      perror("kevent");
    }

    close(dir->wd);
  }
#endif
  dir->wd = -1;
}


static void stat_path(w_root_t *root,
    w_string_t *full_path, struct timeval now, bool confident)
{
  struct stat st;
  int res;
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

  if (res && (errno == ENOENT || errno == ENOTDIR)) {
    /* it's not there, update our state */
    if (dir_ent) {
      w_root_mark_deleted(root, dir_ent, now, true, true);
      w_log(W_LOG_DBG, "lstat(%s) -> %s so stopping watch on %s\n",
          path, strerror(errno), dir_ent->path->buf);
      stop_watching_dir(root, dir_ent);
    }
    if (file) {
      file->exists = false;
      w_root_mark_file_changed(root, file, now, confident);
    }
  } else if (res) {
    w_log(W_LOG_ERR, "lstat(%s) %d %s\n",
        path, errno, strerror(errno));
  } else if (!S_ISDIR(st.st_mode)) {
    if (!file) {
      file = w_root_resolve_file(root, dir, file_name);
    }
    file->exists = true;
    w_root_mark_file_changed(root, file, now, confident);
    memcpy(&file->st, &st, sizeof(st));
  } else if (S_ISDIR(st.st_mode)) {
    if (!dir_ent) {
      /* we've never seen this dir before */
      crawler(root, full_path, now, confident);
    }
  }

  w_string_delref(dir_name);
  w_string_delref(file_name);
}


void w_root_process_path(w_root_t *root, w_string_t *full_path,
    struct timeval now, bool confident)
{
  if (w_string_equal(full_path, root->root_path)) {
    crawler(root, full_path, now, confident);
  } else {
    stat_path(root, full_path, now, confident);
  }
}

/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, bool confident, bool recursive)
{
  w_ht_iter_t i;

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = (struct watchman_file*)i.value;

    if (file->exists) {
      file->exists = false;
      w_root_mark_file_changed(root, file, now, confident);
    }

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    w_root_mark_deleted(root, child, now, confident, recursive);
  } while (w_ht_next(dir->dirs, &i));
}

#if HAVE_INOTIFY_INIT
struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd)
{
  return (struct watchman_dir*)w_ht_get(root->wd_to_dir, wd);
}
#endif

static void crawler(w_root_t *root, w_string_t *dir_name,
    struct timeval now, bool confident)
{
  struct watchman_dir *dir;
  struct watchman_file *file;
  DIR *osdir;
  struct dirent *dirent;
  w_ht_iter_t i;

  dir = w_root_resolve_dir(root, dir_name, true);

  osdir = opendir(dir_name->buf);
  if (!osdir) {
    if (errno == ENOENT || errno == ENOTDIR) {
      w_log(W_LOG_DBG, "opendir(%s) -> %s so stopping watch\n",
          dir_name->buf, strerror(errno));
      stop_watching_dir(root, dir);
      w_root_mark_deleted(root, dir, now, true, true);
    }
    return;
  }

  /* make sure we're watching this guy */
  if (dir->wd == -1) {
#if HAVE_INOTIFY_INIT
    dir->wd = inotify_add_watch(root->infd, dir_name->buf,
        WATCHMAN_INOTIFY_MASK);
    /* record mapping */
    if (dir->wd != -1) {
      w_ht_replace(root->wd_to_dir, dir->wd, (w_ht_val_t)dir);
      w_log(W_LOG_DBG, "adding %d -> %.*s mapping\n",
          dir->wd, dir_name->len, dir_name->buf);
    }
#endif
#if HAVE_KQUEUE
    dir->wd = open(dir_name->buf, O_EVTONLY);
    if (dir->wd != -1) {
      struct kevent k;

      memset(&k, 0, sizeof(k));
      EV_SET(&k, dir->wd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
        NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME,
        0,
        // See consume_kqueue for commentary on this
        // bit setting
        (void*)(((intptr_t)dir) | 0x1));
      w_set_cloexec(dir->wd);

      if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
        perror("kevent");
        close(dir->wd);
        dir->wd = -1;
      }
    }
#endif
  }

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
    if (!file || !file->exists) {
      w_root_add_pending_rel(root, dir, dirent->d_name,
          confident, now, false);
    }
    w_string_delref(name);
  }
  closedir(osdir);

  // Re-examine all the files we think exist
  if (w_ht_first(dir->files, &i)) do {
    file = (struct watchman_file*)i.value;
    if (file->exists) {
      w_root_add_pending_rel(root, dir, file->name->buf,
          confident, now, false);
    }
  } while (w_ht_next(dir->files, &i));

  // if we have any child dirs, add those to the list too
  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    w_root_add_pending(root, child->path, confident, now, false);
  } while (w_ht_next(dir->dirs, &i));
}

static void spawn_command(w_root_t *root,
    struct watchman_trigger_command *cmd,
    uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  char **argv;
  uint32_t argc;
  uint32_t i, j;
  int ret;
  pid_t pid;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  sigset_t mask;

  argc = cmd->argc + num_matches;
  argv = calloc(argc + 1, sizeof(char*));

  /* copy in the base command */
  for (i = 0; i < cmd->argc; i++) {
    argv[i] = cmd->argv[i];
  }

  /* now fill out the file name args */
  for (j = 0; j < num_matches; j++) {
    w_string_t *relname = matches[j].relname;

    argv[i++] = w_string_dup_buf(relname);

    w_string_delref(relname);
  }
  free(matches);

  argv[i] = NULL;

  posix_spawnattr_init(&attr);
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  posix_spawnattr_setflags(&attr,
      POSIX_SPAWN_SETSIGMASK|
      POSIX_SPAWN_SETPGROUP);

  posix_spawn_file_actions_init(&actions);
  // TODO: std{in,out,err} redirection by default?

  ignore_result(chdir(root->root_path->buf));

  ret = posix_spawnp(&pid, argv[0], &actions,
      &attr, argv, environ);

  w_log(W_LOG_DBG, "posix_spawnp: argc=%d\n", argc);
  for (i = 0; i < argc; i++) {
    w_log(W_LOG_DBG, "  [%d] %s\n", i, argv[i]);
  }
  w_log(W_LOG_DBG, "pid=%d ret=%d\n", pid, ret);

  ignore_result(chdir("/"));

  for (i = cmd->argc; i < argc; i++) {
    free(argv[i]);
  }
  free(argv);

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
}

/* process any pending triggers.
 * must be called with root locked
 */
static void process_triggers(w_root_t *root)
{
  struct watchman_file *f, *oldest = NULL;
  struct watchman_rule_match *results = NULL;
  uint32_t matches;
  w_ht_iter_t iter;

  if (root->last_trigger_tick == root->pending_trigger_tick) {
    return;
  }

  w_log(W_LOG_DBG, "last=%" PRIu32 "  pending=%" PRIu32 "\n",
      root->last_trigger_tick,
      root->pending_trigger_tick);

  for (f = root->latest_file;
      f && f->otime.ticks > root->last_trigger_tick;
      f = f->next) {

    oldest = f;
  }

#if 0
  for (f = oldest; f; f = f->prev) {
    w_log(W_LOG_DBG,
        "M %.*s/%.*s exists=%s confident=%s t=%" PRIu32 " s=%" PRIu32 "\n",
        f->parent->path->len,
        f->parent->path->buf,
        f->name->len,
        f->name->buf,
        f->exists ? "true" : "false",
        f->confident ? "true" : "false",
        f->otime.ticks,
        f->otime.seconds);
    w_log(W_LOG_DBG, "f=%p f.n=%p f.p=%p\n", f, f->next, f->prev);
  }
#endif

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd;

    cmd = (struct watchman_trigger_command*)iter.value;
    matches = w_rules_match(root, oldest, &results, cmd->rules);
    if (matches > 0) {
      spawn_command(root, cmd, matches, results);
    }

  } while (w_ht_next(root->commands, &iter));

  root->last_trigger_tick = root->pending_trigger_tick;
}

// For a client to wait for updates to settle out
// Must be called with the root locked
bool w_root_wait_for_settle(w_root_t *root, int settlems)
{
  struct timeval settle, now, target, diff;
  struct timespec ts;
  int res;

  if (settlems == -1) {
    settlems = trigger_settle;
  }

  settle.tv_sec = settlems / 1000;
  settle.tv_usec = settlems - (settle.tv_sec * 1000);

  while (true) {
    gettimeofday(&now, NULL);
    if (root->latest_file) {
      w_timeval_add(root->latest_file->otime.tv, settle, &target);
      if (w_timeval_compare(now, target) >= 0) {
        // We're settled!
        return true;
      }

      // Set the wait time to the difference
      w_timeval_sub(target, now, &diff);
      w_timeval_to_timespec(diff, &ts);

    } else {
      // we don't have any files, so let's wait one round of
      // tick time
      w_timeval_to_timespec(settle, &ts);
    }

    w_root_unlock(root);
    res = nanosleep(&ts, NULL);
    w_root_lock(root);

    if (res == 0 && root->latest_file == NULL) {
      return true;
    }
  }

  return true;
}

static void *stat_thread(void *arg)
{
  w_root_t *root = arg;
  struct timeval start, end, now, target;
  struct timeval settle;

  settle.tv_sec = trigger_settle / 1000;
  settle.tv_usec = trigger_settle - (settle.tv_sec * 1000);

  /* first order of business is to find all the files under our root */
  gettimeofday(&start, NULL);
  w_root_lock(root);
  w_root_add_pending(root, root->root_path, false, start, false);
  w_root_unlock(root);

  /* now we just sit and wait for things to land in our pending list */
  for (;;) {
    int err;

    w_root_lock(root);
    if (!root->pending) {

      // Throttle our trigger rate
      gettimeofday(&now, NULL);
      if (root->latest_file) {
        w_timeval_add(root->latest_file->otime.tv, settle, &target);
        if (w_timeval_compare(now, target) < 0) {
          // Still have a bit of time to wait
          struct timespec ts;

          w_timeval_to_timespec(target, &ts);
          err = pthread_cond_timedwait(&root->cond, &root->lock, &ts);

          if (err != ETIMEDOUT) {
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

      err = pthread_cond_wait(&root->cond, &root->lock);
      if (err != 0) {
        w_log(W_LOG_ERR, "pthread_cond_wait: %s\n",
            strerror(err));
        w_root_lock(root);
      }
    }
have_pending:
    w_root_process_pending(root);
    w_root_unlock(root);
  }
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

  for (i = 0; n > 0 && i < n; i++) {
    /* We leverage the fact that our aligned pointers
     * will never set the LSB of a pointer value.
     * We can use the LSB to indicate whether kqueue
     * entries are dirs or files */
    intptr_t p = (intptr_t)k[i].udata;

    if (p & 0x1) {
      struct watchman_dir *dir = (void*)(p & ~0x1);

      w_log(W_LOG_DBG, " KQ dir %s\n", dir->path->buf);
      w_ht_set(batch, (w_ht_val_t)dir->path, (w_ht_val_t)dir);
    } else {
      struct watchman_file *file = (void*)p;
      w_string_t *name;

      name = w_string_path_cat(file->parent->path, file->name);
      w_ht_set(batch, (w_ht_val_t)name, (w_ht_val_t)file);
      w_log(W_LOG_DBG, " KQ file %s\n", name->buf);
      w_string_delref(name);
    }
  }

  return n;
}

static void *kqueue_thread(void *arg)
{
  w_root_t *root = arg;
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

    w_log(W_LOG_DBG, "Have %d events in %s\n",
        w_ht_size(batch), root->root_path->buf);

    if (w_ht_size(batch)) {
      w_ht_iter_t iter;

      w_root_lock(root);
      root->ticks++;
      gettimeofday(&now, NULL);
      if (w_ht_first(batch, &iter)) do {
        w_string_t *name = (w_string_t*)iter.key;

        w_log(W_LOG_DBG, "kq -> %s\n", name->buf);
        w_root_add_pending(root, name, true, now, true);

      } while (w_ht_next(batch, &iter));

      w_root_unlock(root);

      w_ht_free(batch);
      batch = NULL;
    }
  }
  return NULL;
}

#endif

#if HAVE_INOTIFY_INIT
// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work to the
// stat_thread above.
static void *inotify_thread(void *arg)
{
  w_root_t *root = arg;

  /* now we can settle into the notification stuff */
  for (;;) {
    struct inotify_event *ine;
    char ibuf[WATCHMAN_NAME_MAX];
    char *iptr;
    int n;
    struct timeval now;
    struct watchman_dir *dir;
    w_string_t *name;

    n = read(root->infd, &ibuf, sizeof(ibuf));
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      w_log(W_LOG_ERR, "read(%d, %lu): error %s\n",
          root->infd, sizeof(ibuf), strerror(errno));
      abort();
    }

    w_log(W_LOG_ERR, "inotify read: returned %d.\n", n);

    w_root_lock(root);
    root->ticks++;
    gettimeofday(&now, NULL);

    for (iptr = ibuf; iptr < ibuf + n; iptr = iptr + sizeof(*ine) + ine->len) {
      ine = (struct inotify_event*)iptr;

      w_log(W_LOG_DBG, "notify: wd=%d mask=%x %s\n", ine->wd, ine->mask,
          ine->len > 0 ? ine->name : "");


      if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
        /* we missed something, will need to re-crawl */

        w_log(W_LOG_ERR, "inotify: IN_Q_OVERFLOW, re-crawling %.*s",
            root->root_path->len,
            root->root_path->buf);

        /* assume that everything was deleted,
         * garbage collection style */
        dir = w_root_resolve_dir(root, root->root_path, false);
        w_root_mark_deleted(root, dir, now,
            /*confident=*/false, /*recursive=*/true);

        /* any files we find now are obviously not deleted */
        w_root_add_pending(root, root->root_path, false, now, true);
      } else if (ine->wd != -1) {
        char buf[WATCHMAN_NAME_MAX];

        // If we can't resolve the dir, it's because we already know
        // that it has gone away; we've already marked its contents
        // as deleted.
        dir = w_root_resolve_dir_by_wd(root, ine->wd);
        if (dir) {

          if ((ine->mask & IN_ISDIR) == 0 && ine->len) {
            snprintf(buf, sizeof(buf), "%.*s/%s",
                dir->path->len, dir->path->buf,
                ine->name);
            name = w_string_new(buf);

            dir = w_root_resolve_dir(root, name, false);
            if (dir) {
              // If this is a directory, mark its contents
              // deleted so that we'll find them again
              // during crawl
              w_root_mark_deleted(root, dir, now,
                  /*confident=*/false, /*recursive=*/false);
            }

            w_log(W_LOG_DBG, "add_pending for inotify mask=%x %s\n",
                ine->mask, buf);
            w_root_add_pending(root, name, true, now, true);

            w_string_delref(name);
          } else {
            w_log(W_LOG_DBG, "add_pending for inotify mask=%x %s\n",
                ine->mask, dir->path->buf);
            w_root_add_pending(root, dir->path, true, now, true);
          }
        } else {
          w_log(W_LOG_DBG, "wanted dir %d, but not found\n", ine->wd);
        }
      }

      if (ine->wd != -1 && (ine->mask & IN_IGNORED) == IN_IGNORED) {
        dir = w_root_resolve_dir_by_wd(root, ine->wd);
        if (dir) {
          w_log(W_LOG_DBG, "IN_IGNORED: remove %s\n", dir->path->buf);
          stop_watching_dir(root, dir);
        }
      }
    }

    w_root_unlock(root);

    // TODO: handle IN_DELETE_SELF and/or IN_IGNORED and remove
    // our wd
  }

  return 0;
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

w_root_t *w_root_resolve(const char *filename, bool auto_watch)
{
  pthread_t thr;
  struct watchman_root *root;
  char *watch_path;
  w_string_t *root_str;

  watch_path = w_realpath(filename);

  if (!watch_path) {
    perror(filename);
    return NULL;
  }

  if (!watched_roots) {
    watched_roots = w_ht_new(4, &w_ht_string_funcs);
  }

  root_str = w_string_new(watch_path);

  pthread_mutex_lock(&root_lock);
  root = (w_root_t*)w_ht_get(watched_roots, (w_ht_val_t)root_str);
  pthread_mutex_unlock(&root_lock);
  w_string_delref(root_str);

  if (root || !auto_watch) {
    free(watch_path);
    return root;
  }

  w_log(W_LOG_DBG, "Want to watch %s -> %s\n", filename, watch_path);

  root = w_root_new(watch_path);
  free(watch_path);

  if (!root) {
    return NULL;
  }

  pthread_mutex_lock(&root_lock);
  w_ht_set(watched_roots, (w_ht_val_t)root->root_path, (w_ht_val_t)root);
  pthread_mutex_unlock(&root_lock);

#if HAVE_INOTIFY_INIT
  pthread_create(&thr, NULL, inotify_thread, root);
#endif
#if HAVE_KQUEUE
  pthread_create(&thr, NULL, kqueue_thread, root);
#endif
  pthread_create(&thr, NULL, stat_thread, root);

  return root;
}


/* vim:ts=2:sw=2:et:
 */

