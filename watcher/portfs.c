/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_PORT_CREATE

struct portfs_root_state {
  int port_fd;
  port_event_t portevents[WATCHMAN_BATCH_LIMIT];
};

watchman_global_watcher_t portfs_global_init(void) {
  return NULL;
}

void portfs_global_dtor(watchman_global_watcher_t watcher) {
  unused_parameter(watcher);
}

bool portfs_root_init(watchman_global_watcher_t watcher, w_root_t *root,
    char **errmsg) {
  struct portfs_root_state *state;
  unused_parameter(watcher);

  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

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

void portfs_root_dtor(watchman_global_watcher_t watcher, w_root_t *root) {
  struct portfs_root_state *state = root->watch;
  unused_parameter(watcher);

  if (!state) {
    return;
  }

  close(state->port_fd);
  state->port_fd = -1;

  free(state);
  root->watch = NULL;
}

static void portfs_root_signal_threads(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);
}

static bool portfs_root_start(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);

  return true;
}

static bool portfs_root_start_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  struct portfs_root_state *state = root->watch;
  char buf[WATCHMAN_NAME_MAX];
  unused_parameter(watcher);

  snprintf(buf, sizeof(buf), "%.*s/%.*s",
      file->parent->path->len, file->parent->path->buf,
      file->name->len, file->name->buf);

  w_log(W_LOG_DBG, "watch_file(%s)\n", buf);

  file->port_file.fo_atime = file->st.st_atim;
  file->port_file.fo_mtime = file->st.st_mtim;
  file->port_file.fo_ctime = file->st.st_ctim;
  if (!file->port_file.fo_name) {
    file->port_file.fo_name = strdup(buf);
  }

  port_associate(state->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&file->port_file, WATCHMAN_PORT_EVENTS,
      (void*)file);

  return true;
}

static void portfs_root_stop_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  struct portfs_root_state *state = root->watch;
  unused_parameter(watcher);

  port_dissociate(state->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&file->port_file);
  if (file->port_file.fo_name) {
    free(file->port_file.fo_name);
    file->port_file.fo_name = NULL;
  }
}

static DIR *portfs_root_start_watch_dir(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct portfs_root_state *state = root->watch;
  DIR *osdir;
  struct stat st;
  unused_parameter(watcher);

  osdir = opendir_nofollow(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  if (fstat(dirfd(osdir), &st) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(root, "fstat failed");
    closedir(osdir);
    return NULL;
  }

  dir->port_file.fo_mtime = st.st_atim;
  dir->port_file.fo_mtime = st.st_mtim;
  dir->port_file.fo_ctime = st.st_ctim;
  dir->port_file.fo_name = (char*)dir->path->buf;

  errno = 0;
  if (port_associate(state->port_fd, PORT_SOURCE_FILE,
        (uintptr_t)&dir->port_file, WATCHMAN_PORT_EVENTS,
        SET_DIR_BIT(dir))) {
    w_log(W_LOG_ERR, "port_associate %s %s\n",
        dir->port_file.fo_name, strerror(errno));
  }

  return osdir;
}

static void portfs_root_stop_watch_dir(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_dir *dir) {
  struct portfs_root_state *state = root->watch;
  unused_parameter(watcher);

  port_dissociate(state->port_fd, PORT_SOURCE_FILE,
      (uintptr_t)&dir->port_file);
}

static bool portfs_root_consume_notify(watchman_global_watcher_t watcher,
    w_root_t *root)
{
  struct portfs_root_state *state = root->watch;
  uint_t i, n;
  struct timeval now;
  unused_parameter(watcher);

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

  for (i = 0; i < n; i++) {
    if (IS_DIR_BIT_SET(state->portevents[i].portev_user)) {
      struct watchman_dir *dir = DECODE_DIR(state->portevents[i].portev_user);
      uint32_t pe = state->portevents[i].portev_events;

      w_log(W_LOG_DBG, "port: dir %.*s [0x%x]\n",
          dir->path->len, dir->path->buf, pe);

      if ((pe & (FILE_RENAME_FROM|UNMOUNTED|MOUNTEDOVER|FILE_DELETE))
          && w_string_equal(dir->path, root->root_path)) {

        w_log(W_LOG_ERR,
          "root dir %s has been (re)moved (code 0x%x), canceling watch\n",
          root->root_path->buf, pe);

        w_root_cancel(root);
        return false;
      }
      w_root_add_pending(root, dir->path, false, now, true);

    } else {
      struct watchman_file *file = state->portevents[i].portev_user;
      w_string_t *path;

      path = w_string_path_cat(file->parent->path, file->name);
      w_root_add_pending(root, path, true, now, true);
      w_log(W_LOG_DBG, "port: file %.*s\n", path->len, path->buf);
      w_string_delref(path);
    }
  }

  return true;
}

static bool portfs_root_wait_notify(watchman_global_watcher_t watcher,
    w_root_t *root, int timeoutms) {
  struct portfs_root_state *state = root->watch;
  int n;
  struct pollfd pfd;
  unused_parameter(watcher);

  pfd.fd = state->port_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void portfs_file_free(watchman_global_watcher_t watcher,
    struct watchman_file *file) {
  unused_parameter(watcher);

  if (file->port_file.fo_name) {
    free(file->port_file.fo_name);
  }
}

struct watchman_ops portfs_watcher = {
  "portfs",
  false, // per_file_notifications
  portfs_global_init,
  portfs_global_dtor,
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
