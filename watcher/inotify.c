/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef HAVE_INOTIFY_INIT

struct inot_root_state {
  /* we use one inotify instance per watched root dir */
  int infd;

  /* map of active watch descriptor to a dir */
  w_ht_t *wd_to_dir;

  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf[WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + 256)];
};

/* stored in the wd_to_dir map to indicate that we've detected that this
 * directory has disappeared, but that we might not yet have processed all the
 * events leading to its disappearance.
 *
 * Any directory that was replaced in the wd_to_dir map with
 * dir_pending_ignored will have its wd set to -1. This means that if another
 * directory with the same wd shows up, it's OK to replace this with that. */
static struct watchman_dir dir_pending_ignored;

struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd)
{
  struct inot_root_state *state = root->watch;
  return (struct watchman_dir*)w_ht_val_ptr(w_ht_get(state->wd_to_dir, wd));
}

watchman_global_watcher_t inot_global_init(void) {
  return NULL;
}

void inot_global_dtor(watchman_global_watcher_t watcher) {
  unused_parameter(watcher);
}

static const char *inot_strerror(int err) {
  switch (err) {
    case EMFILE:
      return "The user limit on the total number of inotify "
        "instances has been reached; increase the "
        "fs.inotify.max_user_instances sysctl";
    case ENFILE:
      return "The system limit on the total number of file descriptors "
        "has been reached";
    case ENOMEM:
      return "Insufficient kernel memory is available";
    case ENOSPC:
      return "The user limit on the total number of inotify watches "
        "was reached; increase the fs.inotify.max_user_watches sysctl";
    default:
      return strerror(err);
  }
}

bool inot_root_init(watchman_global_watcher_t watcher, w_root_t *root,
    char **errmsg) {
  struct inot_root_state *state;
  unused_parameter(watcher);

  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

  state->infd = inotify_init();
  if (state->infd == -1) {
    ignore_result(asprintf(errmsg, "watch(%.*s): inotify_init error: %s",
        root->root_path->len, root->root_path->buf, inot_strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(state->infd);
  state->wd_to_dir = w_ht_new(HINT_NUM_DIRS, NULL);

  return true;
}

void inot_root_dtor(watchman_global_watcher_t watcher, w_root_t *root) {
  struct inot_root_state *state = root->watch;
  unused_parameter(watcher);

  if (!state) {
    return;
  }

  close(state->infd);
  state->infd = -1;
  if (state->wd_to_dir) {
    w_ht_free(state->wd_to_dir);
    state->wd_to_dir = NULL;
  }

  free(state);
  root->watch = NULL;
}

static void inot_root_signal_threads(watchman_global_watcher_t watcher,
    w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);
}

static bool inot_root_start(watchman_global_watcher_t watcher, w_root_t *root) {
  unused_parameter(watcher);
  unused_parameter(root);

  return true;
}

static bool inot_root_start_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
  return true;
}

static void inot_root_stop_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
}

static DIR *inot_root_start_watch_dir(watchman_global_watcher_t watcher,
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct inot_root_state *state = root->watch;
  DIR *osdir;
  int newwd;
  unused_parameter(watcher);

  // The directory might be different since the last time we looked at it, so
  // call inotify_add_watch unconditionally.
  newwd = inotify_add_watch(state->infd, path, WATCHMAN_INOTIFY_MASK);
  if (newwd == -1) {
    if (errno == ENOSPC || errno == ENOMEM) {
      // Limits exceeded, no recovery from our perspective
      set_poison_state(root, dir, now, "inotify-add-watch", errno,
          inot_strerror(errno));
    } else {
      handle_open_errno(root, dir, now, "inotify_add_watch", errno,
          inot_strerror(errno));
    }
    return NULL;
  } else if (dir->wd != -1 && dir->wd != newwd) {
    // This can happen when a directory is deleted and then recreated very
    // soon afterwards. e.g. dir->wd is 100, but newwd is 200. We'll still
    // expect old events to come in for wd 100, so blackhole
    // those. stop_watching_dir will mark dir->wd as -1, so the condition
    // right below will be true.
    stop_watching_dir(root, dir);
  }

  if (dir->wd == -1) {
    w_log(W_LOG_DBG, "watch_dir(%s)\n", path);
    dir->wd = newwd;
    // record mapping
    w_ht_replace(state->wd_to_dir, dir->wd, w_ht_ptr_val(dir));
    w_log(W_LOG_DBG, "adding %d -> %s mapping\n", dir->wd, path);
  }

  osdir = opendir_nofollow(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  return osdir;
}

static void inot_root_stop_watch_dir(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_dir *dir) {
  struct inot_root_state *state = root->watch;
  unused_parameter(watcher);

  if (dir->wd == -1) {
    return;
  }

  // Linux removes watches for us at the appropriate times, so just mark the
  // directory pending_ignored. At this point, dir->wd != -1 so a real directory
  // exists in the wd_to_dir map.
  w_ht_replace(state->wd_to_dir, dir->wd, w_ht_ptr_val(&dir_pending_ignored));
  w_log(W_LOG_DBG, "marking %d -> %.*s as pending ignored\n",
      dir->wd, dir->path->len, dir->path->buf);

  dir->wd = -1;
}

static void invalidate_watch_descriptors(w_root_t *root,
    struct watchman_dir *dir)
{
  w_ht_iter_t i;
  struct inot_root_state *state = root->watch;

  if (w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)w_ht_val_ptr(i.value);

    invalidate_watch_descriptors(root, child);
  } while (w_ht_next(dir->dirs, &i));

  if (dir->wd != -1) {
    w_ht_del(state->wd_to_dir, dir->wd);
    dir->wd = -1;
  }
}

static void process_inotify_event(
    w_root_t *root,
    struct inotify_event *ine,
    struct timeval now)
{
  struct watchman_dir *dir;
  struct watchman_file *file = NULL;
  w_string_t *name;
  struct inot_root_state *state = root->watch;

  w_log(W_LOG_DBG, "notify: wd=%d mask=%x %s\n", ine->wd, ine->mask,
      ine->len > 0 ? ine->name : "");

  if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
    /* we missed something, will need to re-crawl */
    w_root_schedule_recrawl(root, "IN_Q_OVERFLOW");
  } else if (ine->wd != -1) {
    char buf[WATCHMAN_NAME_MAX];

    dir = w_root_resolve_dir_by_wd(root, ine->wd);
    if (dir == &dir_pending_ignored) {
      if ((ine->mask & IN_IGNORED) != 0) {
        w_log(W_LOG_DBG, "mask=%x: remove watch %d (pending ignored dir)\n",
            ine->mask, ine->wd);
        w_ht_del(state->wd_to_dir, ine->wd);
      } else {
        w_log(W_LOG_DBG, "mask=%x: pending ignored watch %d, name %.*s",
            ine->mask, ine->wd, ine->len, ine->name);
      }
    } else if (dir) {
      // The hint is that it has gone away, so remove our wd mapping here.
      // We'll queue up a stat of that dir anyway so that we really know
      // the state of that path, but that has to happen /after/ we've
      // removed this watch.
      if ((ine->mask & IN_IGNORED) != 0) {
        w_log(W_LOG_DBG, "mask=%x: remove watch %d %.*s\n", ine->mask,
            dir->wd, dir->path->len, dir->path->buf);
        if (dir->wd != -1) {
          w_ht_del(state->wd_to_dir, dir->wd);
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
          w_root_schedule_recrawl(root, "file missing from internal state");
          w_string_delref(name);
          return;
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
        return;
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

      if ((ine->mask & (IN_UNMOUNT|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF))) {
        w_string_t *pname;

        if (w_string_equal(root->root_path, name)) {
          w_log(W_LOG_ERR,
              "root dir %s has been (re)moved, canceling watch\n",
              root->root_path->buf);
          w_string_delref(name);
          w_root_cancel(root);
          return;
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
      w_log(W_LOG_ERR, "wanted dir %d for mask %x but not found %.*s\n",
          ine->wd, ine->mask, ine->len, ine->name);
      w_root_schedule_recrawl(root, "dir missing from internal state");
    }
  }
}

static bool inot_root_consume_notify(watchman_global_watcher_t watcher,
    w_root_t *root)
{
  struct inot_root_state *state = root->watch;
  struct inotify_event *ine;
  char *iptr;
  int n;
  struct timeval now;
  unused_parameter(watcher);

  n = read(state->infd, &state->ibuf, sizeof(state->ibuf));
  if (n == -1) {
    if (errno == EINTR) {
      return false;
    }
    w_log(W_LOG_FATAL, "read(%d, %zu): error %s\n",
        state->infd, sizeof(state->ibuf), strerror(errno));
  }

  w_log(W_LOG_DBG, "inotify read: returned %d.\n", n);
  gettimeofday(&now, NULL);

  for (iptr = state->ibuf; iptr < state->ibuf + n;
      iptr = iptr + sizeof(*ine) + ine->len) {
    ine = (struct inotify_event*)iptr;

    process_inotify_event(root, ine, now);

    if (root->cancelled) {
      return false;
    }
  }

  return true;
}

static bool inot_root_wait_notify(watchman_global_watcher_t watcher,
    w_root_t *root, int timeoutms) {
  struct inot_root_state *state = root->watch;
  int n;
  struct pollfd pfd;
  unused_parameter(watcher);

  pfd.fd = state->infd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static void inot_file_free(watchman_global_watcher_t watcher,
    struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(file);
}

struct watchman_ops inotify_watcher = {
  "inotify",
  true, // per_file_notifications
  inot_global_init,
  inot_global_dtor,
  inot_root_init,
  inot_root_start,
  inot_root_dtor,
  inot_root_start_watch_file,
  inot_root_stop_watch_file,
  inot_root_start_watch_dir,
  inot_root_stop_watch_dir,
  inot_root_signal_threads,
  inot_root_consume_notify,
  inot_root_wait_notify,
  inot_file_free
};

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
