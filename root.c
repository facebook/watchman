/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Maps pid => root
static w_ht_t *running_kids = NULL;
static w_ht_t *watched_roots = NULL;
static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;

/* small for testing, but should make this greater than the number of dirs we
 * have in our repos to avoid realloc */
#define HINT_NUM_DIRS 16*1024

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
    struct timeval now, bool confident);

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
#if HAVE_PORT_CREATE
  root->port_fd = port_create();
  w_set_cloexec(root->port_fd);
#endif
  root->dirname_to_dir = w_ht_new(HINT_NUM_DIRS, &w_ht_string_funcs);
  root->commands = w_ht_new(2, &trigger_hash_funcs);
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

  w_log(W_LOG_DBG, "add_pending: %s\n", path->buf);

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

static struct watchman_file *w_root_resolve_file(w_root_t *root,
    struct watchman_dir *dir, w_string_t *file_name,
    struct timeval now)
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
  file->ctime.ticks = root->ticks;
  file->ctime.tv = now;
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

#if HAVE_PORT_CREATE
  port_dissociate(root->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&dir->port_file);
#endif

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
      file = w_root_resolve_file(root, dir, file_name, now);
    }
    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = root->ticks;
      file->ctime.tv = now;
    }
    file->exists = true;
    memcpy(&file->st, &st, sizeof(st));
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
        SET_DIR_BIT(dir));
      w_set_cloexec(dir->wd);

      if (kevent(root->kq_fd, &k, 1, NULL, 0, 0)) {
        perror("kevent");
        close(dir->wd);
        dir->wd = -1;
      }
    }
#endif
#if HAVE_PORT_CREATE
    {
      struct stat st;

      lstat(dir_name->buf, &st);
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
/* process any pending triggers.
 * must be called with root locked
 */
static void process_triggers(w_root_t *root)
{
  struct watchman_file *f, *oldest = NULL;
  struct watchman_rule_match *results = NULL;
  uint32_t matches;
  w_ht_iter_t iter;
  struct w_clockspec_query since;

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

  since.is_timestamp = false;
  since.ticks = root->last_trigger_tick;

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
    intptr_t p = (intptr_t)k[i].udata;

    if (IS_DIR_BIT_SET(p)) {
      struct watchman_dir *dir = DECODE_DIR(p);

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

#if HAVE_PORT_CREATE
static void *portfs_thread(void *arg)
{
  w_root_t *root = arg;

  for (;;) {
    port_event_t events[128];
    uint_t i, n;
    struct timeval now;


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

static w_root_t *root_resolve(const char *filename, bool auto_watch,
    bool *created)
{
  struct watchman_root *root;
  char *watch_path;
  w_string_t *root_str;

  *created = false;
  watch_path = w_realpath(filename);

  if (!watch_path) {
    perror(filename);
    return NULL;
  }

  root_str = w_string_new(watch_path);
  pthread_mutex_lock(&root_lock);
  if (!watched_roots) {
    watched_roots = w_ht_new(4, &w_ht_string_funcs);
  }
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

  *created = true;

  pthread_mutex_lock(&root_lock);
  w_ht_set(watched_roots, (w_ht_val_t)root->root_path, (w_ht_val_t)root);
  if (!running_kids) {
    running_kids = w_ht_new(2, NULL);
  }
  pthread_mutex_unlock(&root_lock);

  return root;
}

static bool root_start(w_root_t *root)
{
  pthread_t thr;

#if HAVE_INOTIFY_INIT
  pthread_create(&thr, NULL, inotify_thread, root);
#endif
#if HAVE_KQUEUE
  pthread_create(&thr, NULL, kqueue_thread, root);
#endif
#if HAVE_PORT_CREATE
  pthread_create(&thr, NULL, portfs_thread, root);
#endif
  pthread_create(&thr, NULL, stat_thread, root);

  return root;
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


/* vim:ts=2:sw=2:et:
 */

