/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_KQUEUE
#if !defined(O_EVTONLY)
# define O_EVTONLY O_RDONLY
#endif

struct kqueue_root_state {
  int kq_fd;
  struct kevent keventbuf[WATCHMAN_BATCH_LIMIT];
};

watchman_global_watcher_t kqueue_global_init(void) {
  return NULL;
}

void kqueue_global_dtor(watchman_global_watcher_t watcher) {
  unused_parameter(watcher);
}

bool kqueue_root_init(watchman_global_watcher_t watcher, w_root_t *root,
    char **errmsg) {
  struct kqueue_root_state *state;
  unused_parameter(watcher);

  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

  state->kq_fd = kqueue();
  if (state->kq_fd == -1) {
    ignore_result(asprintf(errmsg, "watch(%.*s): kqueue() error: %s",
        root->root_path->len, root->root_path->buf, strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(state->kq_fd);

  return true;
}

void kqueue_root_dtor(watchman_global_watcher_t watcher, w_root_t *root) {
  struct kqueue_root_state *state = root->watch;
  unused_parameter(watcher);

  if (!state) {
    return;
  }

  close(state->kq_fd);
  state->kq_fd = -1;

  free(state);
  root->watch = NULL;
}

static void kqueue_root_signal_threads(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);
}

static bool kqueue_root_start(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);

  return true;
}

static bool kqueue_root_start_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  struct kqueue_root_state *state = root->watch;
  struct kevent k;
  char buf[WATCHMAN_NAME_MAX];
  unused_parameter(watcher);

  snprintf(buf, sizeof(buf), "%.*s/%.*s",
      file->parent->path->len, file->parent->path->buf,
      file->name->len, file->name->buf);

  w_log(W_LOG_DBG, "watch_file(%s)\n", buf);

  file->kq_fd = open(buf, O_EVTONLY|O_CLOEXEC);
  if (file->kq_fd == -1) {
    w_log(W_LOG_ERR, "failed to open %s O_EVTONLY: %s\n",
        buf, strerror(errno));
    return false;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, file->kq_fd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
      NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME|NOTE_ATTRIB,
      0, file);

  if (kevent(state->kq_fd, &k, 1, NULL, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD file %s failed: %s",
        buf, strerror(errno));
    close(file->kq_fd);
    file->kq_fd = -1;
  }

  return true;
}

static void kqueue_root_stop_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  struct kqueue_root_state *state = root->watch;
  struct kevent k;
  unused_parameter(watcher);

  if (file->kq_fd == -1) {
    return;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, file->kq_fd, EVFILT_VNODE, EV_DELETE, 0, 0, file);
  kevent(state->kq_fd, &k, 1, NULL, 0, 0);
  close(file->kq_fd);
  file->kq_fd = -1;
}

static DIR *kqueue_root_start_watch_dir(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct kqueue_root_state *state = root->watch;
  DIR *osdir;
  struct stat st, osdirst;
  struct kevent k;
  int newwd;
  unused_parameter(watcher);

  osdir = opendir_nofollow(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno);
    return NULL;
  }

  newwd = open(path, O_NOFOLLOW|O_EVTONLY|O_CLOEXEC);

  if (newwd == -1) {
    // directory got deleted between opendir and open
    handle_open_errno(root, dir, now, "open", errno);
    closedir(osdir);
    return NULL;
  }
  if (fstat(newwd, &st) == -1 || fstat(dirfd(osdir), &osdirst) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(root, "fstat failed");
    close(newwd);
    closedir(osdir);
    return NULL;
  }

  if (st.st_dev != osdirst.st_dev || st.st_ino != osdirst.st_ino) {
    // directory got replaced between opendir and open -- at this point its
    // parent's being watched, so we let filesystem events take care of it
    handle_open_errno(root, dir, now, "open", ENOTDIR);
    close(newwd);
    closedir(osdir);
    return NULL;
  }

  dir->wd = newwd;
  memset(&k, 0, sizeof(k));
  EV_SET(&k, dir->wd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
      NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME,
      0, SET_DIR_BIT(dir));

  if (kevent(state->kq_fd, &k, 1, NULL, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD dir %s failed: %s",
        path, strerror(errno));
    close(dir->wd);
    dir->wd = -1;
  }

  return osdir;
}

static void kqueue_root_stop_watch_dir(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_dir *dir) {
  struct kqueue_root_state *state = root->watch;
  struct kevent k;
  unused_parameter(watcher);

  if (dir->wd == -1) {
    return;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, dir->wd, EVFILT_VNODE, EV_DELETE,
      0, 0, SET_DIR_BIT(dir));

  if (kevent(state->kq_fd, &k, 1, NULL, 0, 0)) {
    w_log(W_LOG_ERR, "rm_watch: %d %.*s %s\n",
        dir->wd, dir->path->len, dir->path->buf,
        strerror(errno));
    w_root_schedule_recrawl(root, "rm_watch failed");
  }

  close(dir->wd);
  dir->wd = -1;
}

static bool kqueue_root_consume_notify(watchman_global_watcher_t watcher,
    w_root_t *root)
{
  struct kqueue_root_state *state = root->watch;
  int n;
  int i;
  struct timespec ts = { 0, 0 };
  struct timeval now;
  unused_parameter(watcher);

  errno = 0;

  w_log(W_LOG_DBG, "kqueue(%s)\n", root->root_path->buf);
  n = kevent(state->kq_fd, NULL, 0,
      state->keventbuf, sizeof(state->keventbuf) / sizeof(state->keventbuf[0]),
      &ts);
  w_log(W_LOG_DBG, "consume_kqueue: %s n=%d err=%s\n",
      root->root_path->buf, n, strerror(errno));
  if (root->cancelled) {
    return 0;
  }

  gettimeofday(&now, NULL);
  for (i = 0; n > 0 && i < n; i++) {
    uint32_t fflags = state->keventbuf[i].fflags;

    if (IS_DIR_BIT_SET(state->keventbuf[i].udata)) {
      struct watchman_dir *dir = DECODE_DIR(state->keventbuf[i].udata);

      w_log(W_LOG_DBG, " KQ dir %s [0x%x]\n", dir->path->buf, fflags);
      if ((fflags & (NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE)) &&
          w_string_equal(dir->path, root->root_path)) {
        w_log(W_LOG_ERR,
            "root dir %s has been (re)moved [code 0x%x], canceling watch\n",
            root->root_path->buf, fflags);
        w_root_cancel(root);
        return 0;
      }
      w_root_add_pending(root, dir->path, false, now, false);
    } else {
      // NetBSD defines udata as intptr type, so the cast is necessary
      struct watchman_file *file = (void *)state->keventbuf[i].udata;

      w_string_t *path;

      path = w_string_path_cat(file->parent->path, file->name);
      w_root_add_pending(root, path, true, now, true);
      w_log(W_LOG_DBG, " KQ file %.*s [0x%x]\n", path->len, path->buf, fflags);
      w_string_delref(path);
    }
  }

  return n > 0;
}

static bool kqueue_root_wait_notify(watchman_global_watcher_t watcher,
    w_root_t *root, int timeoutms) {
  struct kqueue_root_state *state = root->watch;
  int n;
  struct pollfd pfd;
  unused_parameter(watcher);

  pfd.fd = state->kq_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void kqueue_file_free(watchman_global_watcher_t watcher,
    struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(file);
}

struct watchman_ops kqueue_watcher = {
  "kqueue",
  false, // per_file_notifications
  kqueue_global_init,
  kqueue_global_dtor,
  kqueue_root_init,
  kqueue_root_start,
  kqueue_root_dtor,
  kqueue_root_start_watch_file,
  kqueue_root_stop_watch_file,
  kqueue_root_start_watch_dir,
  kqueue_root_stop_watch_dir,
  kqueue_root_signal_threads,
  kqueue_root_consume_notify,
  kqueue_root_wait_notify,
  kqueue_file_free
};

#endif // HAVE_KQUEUE

/* vim:ts=2:sw=2:et:
 */
