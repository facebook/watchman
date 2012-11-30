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

static w_ht_t *watched_roots = NULL;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;

/* small for testing, but should make this greater than the number of dirs we
 * have in our repos to avoid realloc */
#define HINT_NUM_DIRS 16*1024

static void crawler(w_root_t *root, w_string_t *dir_name,
    time_t now, bool confident);

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

  root->infd = inotify_init();
  w_set_cloexec(root->infd);
  root->wd_to_dir = w_ht_new(HINT_NUM_DIRS, NULL);
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
    fprintf(stderr, "lock: %s\n",
        strerror(err));
  }
}

void w_root_unlock(w_root_t *root)
{
  int err;

  err = pthread_mutex_unlock(&root->lock);
  if (err != 0) {
    fprintf(stderr, "lock: %s\n",
        strerror(err));
  }
}

bool w_root_add_pending(w_root_t *root, w_string_t *path,
    bool confident, time_t now)
{
  struct watchman_pending_fs *p = calloc(1, sizeof(*p));

  if (!p) {
    return false;
  }

  p->confident = confident;
  p->now = now;
  p->path = path;
  w_string_addref(path);

  p->next = root->pending;
  root->pending = p;
  pthread_cond_signal(&root->cond);

  return true;
}

bool w_root_add_pending_rel(w_root_t *root, struct watchman_dir *dir,
    const char *name, bool confident, time_t now)
{
  char path[WATCHMAN_NAME_MAX];
  w_string_t *path_str;
  bool res;

  snprintf(path, sizeof(path), "%.*s/%s", dir->path->len, dir->path->buf, name);
  path_str = w_string_new(path);

  res = w_root_add_pending(root, path_str, confident, now);

  w_string_delref(path_str);

  return res;
}

bool w_root_process_pending(w_root_t *root)
{
  struct watchman_pending_fs *pending, *p;

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
//  printf("+DIR %s\n", dir_name->buf);

  return dir;
}

void w_root_mark_file_changed(w_root_t *root, struct watchman_file *file,
    time_t now, bool confident)
{
  file->confident = confident;
  file->otime.seconds = (uint32_t)now;
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

  w_ht_set(dir->files, (w_ht_val_t)file->name, (w_ht_val_t)file);

  return file;
}

static void stat_path(w_root_t *root,
    w_string_t *full_path, time_t now, bool confident)
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
    fprintf(stderr, "path %.*s is too big\n", full_path->len, full_path->buf);
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
    }
    if (file) {
      file->exists = false;
      w_root_mark_file_changed(root, file, now, confident);
    }
  } else if (res) {
    fprintf(stderr, "lstat(%s) %d %s\n",
        path, errno, strerror(errno));
  } else if (!S_ISDIR(st.st_mode)) {
    if (!file) {
      file = w_root_resolve_file(root, dir, file_name);
    }
    w_root_mark_file_changed(root, file, now, confident);
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
    time_t now, bool confident)
{
  if (w_string_equal(full_path, root->root_path)) {
    crawler(root, full_path, now, confident);
  } else {
    stat_path(root, full_path, now, confident);
  }
}


/* recursively mark the dir contents as deleted */
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    time_t now, bool confident, bool recursive)
{
  w_ht_iter_t i;

  /* turn off watch */
  if (dir->wd != -1) {
    w_ht_del(root->wd_to_dir, dir->wd);
    inotify_rm_watch(root->infd, dir->wd);
    dir->wd = -1;
  }

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = (struct watchman_file*)i.value;

    file->exists = false;
    w_root_mark_file_changed(root, file, now, confident);

  } while (w_ht_next(dir->files, &i));

  if (recursive && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    w_root_mark_deleted(root, child, now, confident, recursive);
  } while (w_ht_next(dir->dirs, &i));
}

struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd)
{
  return (struct watchman_dir*)w_ht_get(root->wd_to_dir, wd);
}

static void crawler(w_root_t *root, w_string_t *dir_name,
    time_t now, bool confident)
{
  struct watchman_dir *dir;
  DIR *osdir;
  struct dirent *dirent;
  w_ht_iter_t i;

  dir = w_root_resolve_dir(root, dir_name, true);

  osdir = opendir(dir_name->buf);
  if (!osdir) {
    if (errno == ENOENT || errno == ENOTDIR) {
      w_root_mark_deleted(root, dir, now, true, true);
    }
    return;
  }

  /* make sure we're watching this guy */
  if (dir->wd == -1) {
    dir->wd = inotify_add_watch(root->infd, dir_name->buf,
        WATCHMAN_INOTIFY_MASK);
    /* record mapping */
    if (dir->wd != -1) {
      w_ht_set(root->wd_to_dir, dir->wd, (w_ht_val_t)dir);
    }
  }

  while ((dirent = readdir(osdir)) != NULL) {
    // Don't follow parent/self links
    if (dirent->d_name[0] == '.' && (
          !strcmp(dirent->d_name, ".") ||
          !strcmp(dirent->d_name, "..")
        )) {
      continue;
    }

    // Queue it up for analysis
    w_root_add_pending_rel(root, dir, dirent->d_name, confident, now);
  }
  closedir(osdir);

  // if we have any child dirs, add thos to the list too
  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

    w_root_add_pending(root, child->path, confident, now);
  } while (w_ht_next(dir->dirs, &i));
}

static void spawn_command(w_root_t *root,
    struct watchman_trigger_command *cmd,
    w_ht_t *filenames)
{
  char **argv;
  int argc;
  int i;
  int ret;
  pid_t pid;
  w_ht_iter_t iter;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  sigset_t mask;

  argc = cmd->argc + w_ht_size(filenames);
  argv = calloc(argc + 1, sizeof(char*));

  /* copy in the base command */
  for (i = 0; i < cmd->argc; i++) {
    argv[i] = cmd->argv[i];
  }

  /* now fill out the file name args */
  if (w_ht_first(filenames, &iter)) do {
    w_string_t *relname = (w_string_t*)iter.key;

    argv[i++] = w_string_dup_buf(relname);

  } while (w_ht_next(filenames, &iter));

  argv[i] = NULL;

  posix_spawnattr_init(&attr);
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  posix_spawnattr_setflags(&attr,
      POSIX_SPAWN_SETSIGMASK|
      POSIX_SPAWN_SETPGROUP);

  posix_spawn_file_actions_init(&actions);
  // TODO: std{in,out,err} redirection by default?

  chdir(root->root_path->buf);

  ret = posix_spawnp(&pid, argv[0], &actions,
      &attr, argv, environ);

  printf("posix_spawnp: argc=%d\n", argc);
  for (i = 0; i < argc; i++) {
    printf("  [%d] %s\n", i, argv[i]);
  }
  printf("pid=%d ret=%d\n", pid, ret);

  chdir("/");

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
  w_ht_t *matches;
  w_ht_iter_t iter;

  if (root->last_trigger_tick == root->pending_trigger_tick) {
    return;
  }

  printf("last=%" PRIu32 "  pending=%" PRIu32 "\n",
      root->last_trigger_tick,
      root->pending_trigger_tick);

  for (f = root->latest_file;
      f && f->otime.ticks > root->last_trigger_tick;
      f = f->next) {

    oldest = f;
  }

#if 0
  for (f = oldest; f; f = f->prev) {
    printf(
        "M %.*s/%.*s exists=%s confident=%s t=%" PRIu32 " s=%" PRIu32 "\n",
        f->parent->path->len,
        f->parent->path->buf,
        f->name->len,
        f->name->buf,
        f->exists ? "true" : "false",
        f->confident ? "true" : "false",
        f->otime.ticks,
        f->otime.seconds);
    printf("f=%p f.n=%p f.p=%p\n", f, f->next, f->prev);
  }
#endif

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd;

    cmd = (struct watchman_trigger_command*)iter.value;
    matches = w_ht_new(8, &w_ht_string_funcs);
    if (w_rules_match(root, oldest, matches, cmd->rules)) {
      spawn_command(root, cmd, matches);
    }
    w_ht_free(matches);

  } while (w_ht_next(root->commands, &iter));

  root->last_trigger_tick = root->pending_trigger_tick;
}

static void *stat_thread(void *arg)
{
  w_root_t *root = arg;
  struct timeval start, end;

  /* first order of business is to find all the files under our root */
  gettimeofday(&start, NULL);
  w_root_lock(root);
  w_root_add_pending(root, root->root_path, false, start.tv_sec);
  w_root_unlock(root);

  /* now we just sit and wait for things to land in our pending list */
  for (;;) {
    int err;

    w_root_lock(root);
    if (!root->pending) {
      if (!root->done_initial) {
        gettimeofday(&end, NULL);
        printf("%s scanned in %.2f seconds\n", root->root_path->buf,
            time_diff(start, end));
        root->done_initial = true;
        w_string_collect();
      }

      process_triggers(root);

      err = pthread_cond_wait(&root->cond, &root->lock);
      if (err != 0) {
        fprintf(stderr, "pthread_cond_wait: %s\n",
            strerror(err));
        w_root_lock(root);
      }
    }
    w_root_process_pending(root);
    w_root_unlock(root);
  }
  return 0;
}

// we want to consume inotify events as quickly as possible
// to minimize the risk that the kernel event buffer overflows,
// so we do this as a blocking thread that reads the inotify
// descriptor and then queues the filesystem IO work to the
// stat_thread above.
static void *notify_thread(void *arg)
{
  w_root_t *root = arg;

  /* now we can settle into the notification stuff */
  for (;;) {
    struct {
      struct inotify_event ine;
      char namebuf[1024];
    } ibuf;
    int n;
    time_t now;
    struct watchman_dir *dir;
    w_string_t *name;

    n = read(root->infd, &ibuf, sizeof(ibuf));
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "read(%d, %lu): error %s\n",
          root->infd, sizeof(ibuf), strerror(errno));
      abort();
    }

    if (n < sizeof(ibuf.ine)) {
      fprintf(stderr, "expected to read sizeof(ine) %lu, but got %d\n",
          sizeof(ibuf.ine), n);
      continue;
    }

    printf("notify: wd=%d mask=%x %s\n", ibuf.ine.wd, ibuf.ine.mask,
        ibuf.ine.len > 0 ? ibuf.ine.name : "");

    root->ticks++;
    time(&now);

    w_root_lock(root);
    if (ibuf.ine.wd == -1 && (ibuf.ine.mask & IN_Q_OVERFLOW)) {
      /* we missed something, will need to re-crawl */

      /* assume that everything was deleted,
       * garbage collection style */
      dir = w_root_resolve_dir(root, root->root_path, false);
      w_root_mark_deleted(root, dir, now,
          /*confident=*/false, /*recursive=*/true);

      /* any files we find now are obviously not deleted */
      w_root_add_pending(root, root->root_path, false, now);
    } else if (ibuf.ine.wd != -1 && ibuf.ine.len) {
      char buf[WATCHMAN_NAME_MAX];

      dir = w_root_resolve_dir_by_wd(root, ibuf.ine.wd);

      snprintf(buf, sizeof(buf), "%.*s/%s",
          dir->path->len, dir->path->buf,
          ibuf.ine.name);
      name = w_string_new(buf);

      dir = w_root_resolve_dir(root, name, false);
      if (dir) {
        // If this is a directory, mark its contents
        // deleted so that we'll find them again
        // during crawl
        w_root_mark_deleted(root, dir, now,
          /*confident=*/false, /*recursive=*/false);
      }

      w_root_add_pending(root, name, true, now);

      w_string_delref(name);
    }
    w_root_unlock(root);

    // TODO: handle IN_DELETE_SELF and/or IN_IGNORED and remove
    // our wd
  }

  return 0;
}

w_root_t *w_root_resolve(const char *filename, bool auto_watch)
{
  pthread_t thr;
  struct watchman_root *root;
  char true_path[WATCHMAN_NAME_MAX];
  char *watch_path;
  w_string_t *root_str;

  watch_path = realpath(filename, true_path);
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
    return root;
  }

  printf("Want to watch %s -> %s\n", filename, watch_path);

  root = w_root_new(watch_path);
  if (!root) {
    return NULL;
  }

  pthread_mutex_lock(&root_lock);
  w_ht_set(watched_roots, (w_ht_val_t)root->root_path, (w_ht_val_t)root);
  pthread_mutex_unlock(&root_lock);

  pthread_create(&thr, NULL, notify_thread, root);
  pthread_create(&thr, NULL, stat_thread, root);

  return root;
}


/* vim:ts=2:sw=2:et:
 */

