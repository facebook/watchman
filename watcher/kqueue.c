/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_KQUEUE
#if !defined(O_EVTONLY)
# define O_EVTONLY O_RDONLY
#endif

struct kqueue_root_state {
  int kq_fd;
  /* map of active watch descriptor to name of the corresponding item */
  w_ht_t *name_to_fd;
  w_ht_t *fd_to_name;
  /* lock to protect the map above */
  pthread_mutex_t lock;
  struct kevent keventbuf[WATCHMAN_BATCH_LIMIT];
};

static const struct flag_map kflags[] = {
  {NOTE_DELETE, "NOTE_DELETE"},
  {NOTE_WRITE, "NOTE_WRITE"},
  {NOTE_EXTEND, "NOTE_EXTEND"},
  {NOTE_ATTRIB, "NOTE_ATTRIB"},
  {NOTE_LINK, "NOTE_LINK"},
  {NOTE_RENAME, "NOTE_RENAME"},
  {NOTE_REVOKE, "NOTE_REVOKE"},
  {0, NULL},
};

static void kqueue_del_key(w_ht_val_t key) {
  w_log(W_LOG_DBG, "KQ close fd=%d\n", (int)key);
  close(key);
}

const struct watchman_hash_funcs name_to_fd_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL, // copy_val
  kqueue_del_key,
};

bool kqueue_root_init(w_root_t *root, char **errmsg) {
  struct kqueue_root_state *state;
  json_int_t hint_num_dirs =
      cfg_get_int(root, CFG_HINT_NUM_DIRS, HINT_NUM_DIRS);

  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;
  pthread_mutex_init(&state->lock, NULL);
  state->name_to_fd = w_ht_new(hint_num_dirs, &name_to_fd_funcs);
  state->fd_to_name = w_ht_new(hint_num_dirs, &w_ht_string_val_funcs);

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

void kqueue_root_dtor(w_root_t *root) {
  struct kqueue_root_state *state = root->watch;

  if (!state) {
    return;
  }

  pthread_mutex_destroy(&state->lock);
  close(state->kq_fd);
  state->kq_fd = -1;
  w_ht_free(state->name_to_fd);
  w_ht_free(state->fd_to_name);

  free(state);
  root->watch = NULL;
}

static void kqueue_root_signal_threads(w_root_t *root) {
  unused_parameter(root);
}

static bool kqueue_root_start(w_root_t *root) {
  unused_parameter(root);

  return true;
}

static bool kqueue_root_start_watch_file(
      w_root_t *root, struct watchman_file *file) {
  struct kqueue_root_state *state = root->watch;
  struct kevent k;
  w_ht_val_t fdval;
  int fd;
  w_string_t *full_name;

  full_name = w_dir_path_cat_str(file->parent, w_file_get_name(file));
  pthread_mutex_lock(&state->lock);
  if (w_ht_lookup(state->name_to_fd, w_ht_ptr_val(full_name), &fdval, false)) {
    // Already watching it
    pthread_mutex_unlock(&state->lock);
    w_string_delref(full_name);
    return true;
  }
  pthread_mutex_unlock(&state->lock);

  w_log(W_LOG_DBG, "watch_file(%s)\n", full_name->buf);

  fd = open(full_name->buf, O_EVTONLY|O_CLOEXEC);
  if (fd == -1) {
    w_log(W_LOG_ERR, "failed to open %s O_EVTONLY: %s\n",
        full_name->buf, strerror(errno));
    w_string_delref(full_name);
    return false;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(&k, fd, EVFILT_VNODE, EV_ADD|EV_CLEAR,
      NOTE_WRITE|NOTE_DELETE|NOTE_EXTEND|NOTE_RENAME|NOTE_ATTRIB,
      0, full_name);

  pthread_mutex_lock(&state->lock);
  w_ht_replace(state->name_to_fd, w_ht_ptr_val(full_name), fd);
  w_ht_replace(state->fd_to_name, fd, w_ht_ptr_val(full_name));
  pthread_mutex_unlock(&state->lock);

  if (kevent(state->kq_fd, &k, 1, NULL, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD file %s failed: %s",
        full_name->buf, strerror(errno));
    close(fd);
    pthread_mutex_lock(&state->lock);
    w_ht_del(state->name_to_fd, w_ht_ptr_val(full_name));
    w_ht_del(state->fd_to_name, fd);
    pthread_mutex_unlock(&state->lock);
  } else {
    w_log(W_LOG_DBG, "kevent file %s -> %d\n", full_name->buf, fd);
  }
  w_string_delref(full_name);

  return true;
}

static void kqueue_root_stop_watch_file(
    w_root_t *root, struct watchman_file *file) {
  unused_parameter(root);
  unused_parameter(file);
}

static struct watchman_dir_handle *kqueue_root_start_watch_dir(
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct kqueue_root_state *state = root->watch;
  struct watchman_dir_handle *osdir;
  struct stat st, osdirst;
  struct kevent k;
  int newwd;
  w_string_t *dir_name;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  newwd = open(path, O_NOFOLLOW|O_EVTONLY|O_CLOEXEC);

  if (newwd == -1) {
    // directory got deleted between opendir and open
    handle_open_errno(root, dir, now, "open", errno, NULL);
    w_dir_close(osdir);
    return NULL;
  }
  if (fstat(newwd, &st) == -1 || fstat(w_dir_fd(osdir), &osdirst) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    w_root_schedule_recrawl(root, "fstat failed");
    close(newwd);
    w_dir_close(osdir);
    return NULL;
  }

  if (st.st_dev != osdirst.st_dev || st.st_ino != osdirst.st_ino) {
    // directory got replaced between opendir and open -- at this point its
    // parent's being watched, so we let filesystem events take care of it
    handle_open_errno(root, dir, now, "open", ENOTDIR, NULL);
    close(newwd);
    w_dir_close(osdir);
    return NULL;
  }

  memset(&k, 0, sizeof(k));
  dir_name = w_dir_copy_full_path(dir);
  EV_SET(&k, newwd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME, 0,
         SET_DIR_BIT(dir_name));

  // Our mapping needs to be visible before we add it to the queue,
  // otherwise we can get a wakeup and not know what it is
  pthread_mutex_lock(&state->lock);
  w_ht_replace(state->name_to_fd, w_ht_ptr_val(dir_name), newwd);
  w_ht_replace(state->fd_to_name, newwd, w_ht_ptr_val(dir_name));
  pthread_mutex_unlock(&state->lock);

  if (kevent(state->kq_fd, &k, 1, NULL, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD dir %s failed: %s",
        path, strerror(errno));
    close(newwd);

    pthread_mutex_lock(&state->lock);
    w_ht_del(state->name_to_fd, w_ht_ptr_val(dir_name));
    w_ht_del(state->fd_to_name, newwd);
    pthread_mutex_unlock(&state->lock);
  } else {
    w_log(W_LOG_DBG, "kevent dir %s -> %d\n", dir_name->buf, newwd);
  }
  w_string_delref(dir_name);

  return osdir;
}

static void kqueue_root_stop_watch_dir(
    w_root_t *root, struct watchman_dir *dir) {
  unused_parameter(root);
  unused_parameter(dir);
}

static bool kqueue_root_consume_notify(
    w_root_t *root, struct watchman_pending_collection *coll)
{
  struct kqueue_root_state *state = root->watch;
  int n;
  int i;
  struct timespec ts = { 0, 0 };
  struct timeval now;

  errno = 0;
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
    bool is_dir = IS_DIR_BIT_SET(state->keventbuf[i].udata);
    w_string_t *path;
    char flags_label[128];
    int fd = state->keventbuf[i].ident;

    w_expand_flags(kflags, fflags, flags_label, sizeof(flags_label));
    pthread_mutex_lock(&state->lock);
    path = w_ht_val_ptr(w_ht_get(state->fd_to_name, fd));
    if (!path) {
      // Was likely a buffered notification for something that we decided
      // to stop watching
      w_log(W_LOG_DBG,
          " KQ notif for fd=%d; flags=0x%x %s no ref for it in fd_to_name\n",
          fd, fflags, flags_label);
      pthread_mutex_unlock(&state->lock);
      continue;
    }
    w_string_addref(path);

    w_log(W_LOG_DBG, " KQ fd=%d path %s [0x%x %s]\n",
        fd, path->buf, fflags, flags_label);
    if ((fflags & (NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE))) {
      struct kevent k;

      if (w_string_equal(path, root->root_path)) {
        w_log(W_LOG_ERR,
            "root dir %s has been (re)moved [code 0x%x], canceling watch\n",
            root->root_path->buf, fflags);
        w_root_cancel(root);
        pthread_mutex_unlock(&state->lock);
        return 0;
      }

      // Remove our watch bits
      memset(&k, 0, sizeof(k));
      EV_SET(&k, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
      kevent(state->kq_fd, &k, 1, NULL, 0, 0);
      w_ht_del(state->name_to_fd, w_ht_ptr_val(path));
      w_ht_del(state->fd_to_name, fd);
    }

    pthread_mutex_unlock(&state->lock);
    w_pending_coll_add(coll, path, now,
        is_dir ? 0 : (W_PENDING_RECURSIVE|W_PENDING_VIA_NOTIFY));
    w_string_delref(path);
  }

  return n > 0;
}

static bool kqueue_root_wait_notify(
    w_root_t *root, int timeoutms) {
  struct kqueue_root_state *state = root->watch;
  int n;
  struct pollfd pfd;

  pfd.fd = state->kq_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void kqueue_file_free(struct watchman_file *file) {
  unused_parameter(file);
}

struct watchman_ops kqueue_watcher = {
  "kqueue",
  0,
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
