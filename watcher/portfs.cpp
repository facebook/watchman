/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_PORT_CREATE

#define WATCHMAN_PORT_EVENTS \
  FILE_MODIFIED | FILE_ATTRIB | FILE_NOFOLLOW


struct portfs_root_state {
  int port_fd;
  /* map of file name to watchman_port_file */
  w_ht_t *port_files;
  /* protects port_files */
  pthread_mutex_t lock;
  port_event_t portevents[WATCHMAN_BATCH_LIMIT];
};

struct watchman_port_file {
  file_obj_t port_file;
  w_string_t *name;
};

static const struct flag_map pflags[] = {
  {FILE_ACCESS, "FILE_ACCESS"},
  {FILE_MODIFIED, "FILE_MODIFIED"},
  {FILE_ATTRIB, "FILE_ATTRIB"},
  {FILE_DELETE, "FILE_DELETE"},
  {FILE_RENAME_TO, "FILE_RENAME_TO"},
  {FILE_RENAME_FROM, "FILE_RENAME_FROM"},
  {UNMOUNTED, "UNMOUNTED"},
  {MOUNTEDOVER, "MOUNTEDOVER"},
  {0, NULL},
};

static struct watchman_port_file *make_port_file(w_string_t *name,
    struct stat *st) {
  struct watchman_port_file *f;

  f = (watchman_port_file*)calloc(1, sizeof(*f));
  if (!f) {
    return NULL;
  }
  f->name = name;
  w_string_addref(name);
  f->port_file.fo_name = (char*)name->buf;
  f->port_file.fo_atime = st->st_atim;
  f->port_file.fo_mtime = st->st_mtim;
  f->port_file.fo_ctime = st->st_ctim;

  return f;
}

static void free_port_file(struct watchman_port_file *f) {
  w_string_delref(f->name);
  free(f);
}

static void portfs_del_port_file(w_ht_val_t key) {
  free_port_file((watchman_port_file*)w_ht_val_ptr(key));
}

const struct watchman_hash_funcs port_file_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL, // copy_val
  portfs_del_port_file,
};

bool portfs_root_init(w_root_t *root, char **errmsg) {
  struct portfs_root_state *state;

  state = (portfs_root_state*)calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

  pthread_mutex_init(&state->lock, NULL);
  state->port_files = w_ht_new(
      cfg_get_int(root, CFG_HINT_NUM_DIRS, HINT_NUM_DIRS), &port_file_funcs);

  state->port_fd = port_create();
  if (state->port_fd == -1) {
    ignore_result(asprintf(errmsg, "watch(%.*s): port_create() error: %s",
        root->root_path->len, root->root_path->buf, strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(state->port_fd);

  return true;
}

void portfs_root_dtor(w_root_t *root) {
  auto state = (portfs_root_state*)root->watch;

  if (!state) {
    return;
  }

  close(state->port_fd);
  state->port_fd = -1;
  w_ht_free(state->port_files);
  pthread_mutex_destroy(&state->lock);

  free(state);
  root->watch = NULL;
}

static void portfs_root_signal_threads(w_root_t *root) {
  unused_parameter(root);
}

static bool portfs_root_start(w_root_t *root) {
  unused_parameter(root);

  return true;
}

static bool do_watch(struct portfs_root_state *state, w_string_t *name,
    struct stat *st) {
  struct watchman_port_file *f;
  bool success = false;

  pthread_mutex_lock(&state->lock);
  if (w_ht_get(state->port_files, w_ht_ptr_val(name))) {
    // Already watching it
    success = true;
    goto out;
  }

  f = make_port_file(name, st);
  if (!f) {
    goto out;
  }

  if (!w_ht_set(state->port_files, w_ht_ptr_val(name), w_ht_ptr_val(f))) {
    free_port_file(f);
    goto out;
  }

  w_log(W_LOG_DBG, "watching %s\n", name->buf);
  errno = 0;
  if (port_associate(state->port_fd, PORT_SOURCE_FILE,
        (uintptr_t)&f->port_file, WATCHMAN_PORT_EVENTS,
        (void*)f)) {
    w_log(W_LOG_ERR, "port_associate %s %s\n",
        f->port_file.fo_name, strerror(errno));
    w_ht_del(state->port_files, w_ht_ptr_val(name));
    goto out;
  }

  success = true;

out:
  pthread_mutex_unlock(&state->lock);
  return success;
}

static bool portfs_root_start_watch_file(w_root_t *root,
    struct watchman_file *file) {
  auto state = (portfs_root_state*)root->watch;
  w_string_t *name;
  bool success = false;

  name = w_string_path_cat(file->parent->path, file->name);
  if (!name) {
    return false;
  }
  success = do_watch(state, name, &file->st);
  w_string_delref(name);

  return success;
}

static void portfs_root_stop_watch_file(w_root_t *root,
    struct watchman_file *file) {
  unused_parameter(root);
  unused_parameter(file);
}

static struct watchman_dir_handle *portfs_root_start_watch_dir(
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  auto state = (portfs_root_state*)root->watch;
  struct watchman_dir_handle *osdir;
  struct stat st;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  if (fstat(dirfd(osdir), &st) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(root, "fstat failed");
    w_dir_close(osdir);
    return NULL;
  }

  if (!do_watch(state, dir->path, &st)) {
    w_dir_close(osdir);
    return NULL;
  }

  return osdir;
}

static void portfs_root_stop_watch_dir(w_root_t *root,
    struct watchman_dir *dir) {
  unused_parameter(root);
  unused_parameter(dir);
}

static bool portfs_root_consume_notify(w_root_t *root,
    struct watchman_pending_collection *coll)
{
  auto state = (portfs_root_state*)root->watch;
  uint_t i, n;
  struct timeval now;

  errno = 0;

  n = 1;
  if (port_getn(state->port_fd, state->portevents,
        sizeof(state->portevents) / sizeof(state->portevents[0]), &n, NULL)) {
    if (errno == EINTR) {
      return false;
    }
    w_log(W_LOG_FATAL, "port_getn: %s\n",
        strerror(errno));
  }

  w_log(W_LOG_DBG, "port_getn: n=%u\n", n);

  if (n == 0) {
    return false;
  }

  pthread_mutex_lock(&state->lock);

  for (i = 0; i < n; i++) {
    struct watchman_port_file *f;
    uint32_t pe = state->portevents[i].portev_events;
    char flags_label[128];

    f = (struct watchman_port_file*)state->portevents[i].portev_user;
    w_expand_flags(pflags, pe, flags_label, sizeof(flags_label));
    w_log(W_LOG_DBG, "port: %s [0x%x %s]\n",
        f->port_file.fo_name,
        pe, flags_label);

    if ((pe & (FILE_RENAME_FROM|UNMOUNTED|MOUNTEDOVER|FILE_DELETE))
        && w_string_equal(f->name, root->root_path)) {

      w_log(W_LOG_ERR,
        "root dir %s has been (re)moved (code 0x%x %s), canceling watch\n",
        root->root_path->buf, pe, flags_label);

      w_root_cancel(root);
      pthread_mutex_unlock(&state->lock);
      return false;
    }
    w_pending_coll_add(coll, f->name, now,
        W_PENDING_RECURSIVE|W_PENDING_VIA_NOTIFY);

    // It was port_dissociate'd implicitly.  We'll re-establish a
    // watch later when portfs_root_start_watch_(file|dir) are called again
    w_ht_del(state->port_files, w_ht_ptr_val(f->name));
  }
  pthread_mutex_unlock(&state->lock);

  return true;
}

static bool portfs_root_wait_notify(w_root_t *root, int timeoutms) {
  auto state = (portfs_root_state*)root->watch;
  int n;
  struct pollfd pfd;

  pfd.fd = state->port_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void portfs_file_free(struct watchman_file *file) {
  unused_parameter(file);
}

struct watchman_ops portfs_watcher = {
  "portfs",
  0,
  portfs_root_init,
  portfs_root_start,
  portfs_root_dtor,
  portfs_root_start_watch_file,
  portfs_root_stop_watch_file,
  portfs_root_start_watch_dir,
  portfs_root_stop_watch_dir,
  portfs_root_signal_threads,
  portfs_root_consume_notify,
  portfs_root_wait_notify,
  portfs_file_free
};

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
