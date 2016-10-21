/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "make_unique.h"
#include "watchman.h"

#ifdef HAVE_INOTIFY_INIT

#ifndef IN_EXCL_UNLINK
/* defined in <linux/inotify.h> but we can't include that without
 * breaking userspace */
# define WATCHMAN_IN_EXCL_UNLINK 0x04000000
#else
# define WATCHMAN_IN_EXCL_UNLINK IN_EXCL_UNLINK
#endif

#define WATCHMAN_INOTIFY_MASK \
  IN_ATTRIB | IN_CREATE | IN_DELETE | \
  IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR | WATCHMAN_IN_EXCL_UNLINK

static const struct flag_map inflags[] = {
    {IN_ACCESS, "IN_ACCESS"},
    {IN_MODIFY, "IN_MODIFY"},
    {IN_ATTRIB, "IN_ATTRIB"},
    {IN_CLOSE_WRITE, "IN_CLOSE_WRITE"},
    {IN_CLOSE_NOWRITE, "IN_CLOSE_NOWRITE"},
    {IN_OPEN, "IN_OPEN"},
    {IN_MOVED_FROM, "IN_MOVED_FROM"},
    {IN_MOVED_TO, "IN_MOVED_TO"},
    {IN_CREATE, "IN_CREATE"},
    {IN_DELETE, "IN_DELETE"},
    {IN_DELETE_SELF, "IN_DELETE_SELF"},
    {IN_MOVE_SELF, "IN_MOVE_SELF"},
    {IN_UNMOUNT, "IN_UNMOUNT"},
    {IN_Q_OVERFLOW, "IN_Q_OVERFLOW"},
    {IN_IGNORED, "IN_IGNORED"},
    {IN_ISDIR, "IN_ISDIR"},
    {0, nullptr},
};

struct pending_move {
  time_t created;
  w_string_t *name;
};

struct InotifyWatcher : public Watcher {
  /* we use one inotify instance per watched root dir */
  int infd;

  struct maps {
    /* map of active watch descriptor to name of the corresponding dir */
    w_ht_t* wd_to_name{nullptr};
    /* map of inotify cookie to corresponding name */
    w_ht_t* move_map{nullptr};

    ~maps() {
      if (wd_to_name) {
        w_ht_free(wd_to_name);
      }
      if (move_map) {
        w_ht_free(move_map);
      }
    }
  };

  watchman::Synchronized<maps> maps;

  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf[WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + 256)];

  InotifyWatcher() : Watcher("inotify", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {}
  ~InotifyWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;

  struct watchman_dir_handle* startWatchDir(
      struct write_locked_watchman_root* lock,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  void stopWatchDir(
      struct write_locked_watchman_root* lock,
      struct watchman_dir* dir) override;

  bool consumeNotify(w_root_t* root, struct watchman_pending_collection* coll)
      override;

  bool waitNotify(int timeoutms) override;

  void process_inotify_event(
      w_root_t* root,
      struct watchman_pending_collection* coll,
      struct inotify_event* ine,
      struct timeval now);
};

static w_ht_val_t copy_pending(w_ht_val_t key) {
  auto src = (pending_move*)w_ht_val_ptr(key);
  struct pending_move *dest = (pending_move*)malloc(sizeof(*dest));
  dest->created = src->created;
  dest->name = src->name;
  w_string_addref(src->name);
  return w_ht_ptr_val(dest);
}

static void del_pending(w_ht_val_t key) {
  auto p = (pending_move*)w_ht_val_ptr(key);

  w_string_delref(p->name);
  free(p);
}

static const struct watchman_hash_funcs move_hash_funcs = {
    nullptr, // copy_key
    nullptr, // del_key
    nullptr, // equal_key
    nullptr, // hash_key
    copy_pending, // copy_val
    del_pending // del_val
};

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

bool InotifyWatcher::initNew(w_root_t* root, char** errmsg) {
  auto watcher = watchman::make_unique<InotifyWatcher>();

  if (!watcher) {
    *errmsg = strdup("out of memory");
    return false;
  }

#ifdef HAVE_INOTIFY_INIT1
  watcher->infd = inotify_init1(IN_CLOEXEC);
#else
  watcher->infd = inotify_init();
#endif
  if (watcher->infd == -1) {
    ignore_result(asprintf(
        errmsg,
        "watch(%s): inotify_init error: %s",
        root->root_path.c_str(),
        inot_strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(watcher->infd);

  {
    auto maps = watcher->maps.wlock();
    maps->wd_to_name = w_ht_new(
        root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS),
        &w_ht_string_val_funcs);
    maps->move_map = w_ht_new(2, &move_hash_funcs);
  }

  root->inner.watcher = std::move(watcher);
  return true;
}

InotifyWatcher::~InotifyWatcher() {
  close(infd);
}

struct watchman_dir_handle* InotifyWatcher::startWatchDir(
    struct write_locked_watchman_root* lock,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle* osdir = nullptr;
  int newwd, err;

  // Carry out our very strict opendir first to ensure that we're not
  // traversing symlinks in the context of this root
  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(lock, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  w_string dir_name(path, W_STRING_BYTE);

  // The directory might be different since the last time we looked at it, so
  // call inotify_add_watch unconditionally.
  newwd = inotify_add_watch(infd, path, WATCHMAN_INOTIFY_MASK);
  if (newwd == -1) {
    err = errno;
    if (errno == ENOSPC || errno == ENOMEM) {
      // Limits exceeded, no recovery from our perspective
      set_poison_state(dir_name, now, "inotify-add-watch", errno,
                       inot_strerror(errno));
    } else {
      handle_open_errno(lock, dir, now, "inotify_add_watch", errno,
          inot_strerror(errno));
    }
    w_dir_close(osdir);
    errno = err;
    return nullptr;
  }

  // record mapping
  {
    auto wlock = maps.wlock();
    w_ht_replace(wlock->wd_to_name, newwd, w_ht_ptr_val(dir_name));
  }
  w_log(W_LOG_DBG, "adding %d -> %s mapping\n", newwd, path);

  return osdir;
}

void InotifyWatcher::stopWatchDir(
    struct write_locked_watchman_root*,
    struct watchman_dir*) {
  // Linux removes watches for us at the appropriate times,
  // and tells us about it via inotify, so we have nothing to do here
}

void InotifyWatcher::process_inotify_event(
    w_root_t* root,
    struct watchman_pending_collection* coll,
    struct inotify_event* ine,
    struct timeval now) {
  char flags_label[128];

  w_expand_flags(inflags, ine->mask, flags_label, sizeof(flags_label));
  w_log(W_LOG_DBG, "notify: wd=%d mask=0x%x %s %s\n", ine->wd, ine->mask,
      flags_label, ine->len > 0 ? ine->name : "");

  if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
    /* we missed something, will need to re-crawl */
    w_root_schedule_recrawl(root, "IN_Q_OVERFLOW");
  } else if (ine->wd != -1) {
    w_string_t* name = nullptr;
    char buf[WATCHMAN_NAME_MAX];
    int pending_flags = W_PENDING_VIA_NOTIFY;
    w_string dir_name;

    {
      auto rlock = maps.rlock();
      dir_name =
          (w_string_t*)w_ht_val_ptr(w_ht_get(rlock->wd_to_name, ine->wd));
    }

    if (dir_name) {
      if (ine->len > 0) {
        snprintf(
            buf,
            sizeof(buf),
            "%.*s/%s",
            int(dir_name.size()),
            dir_name.data(),
            ine->name);
        name = w_string_new_typed(buf, W_STRING_BYTE);
      } else {
        name = dir_name;
        w_string_addref(name);
      }
    }

    if (ine->len > 0 && (ine->mask & (IN_MOVED_FROM|IN_ISDIR))
        == (IN_MOVED_FROM|IN_ISDIR)) {
      struct pending_move mv;

      // record this as a pending move, so that we can automatically
      // watch the target when we get the other side of it.
      mv.created = now.tv_sec;
      mv.name = name;

      {
        auto wlock = maps.wlock();
        if (!w_ht_replace(wlock->move_map, ine->cookie, w_ht_ptr_val(&mv))) {
          w_log(
              W_LOG_FATAL,
              "failed to store %" PRIx32 " -> %s in move map\n",
              ine->cookie,
              name->buf);
        }
      }

      w_log(W_LOG_DBG,
          "recording move_from %" PRIx32 " %s\n", ine->cookie,
          name->buf);
    }

    if (ine->len > 0 &&
        (ine->mask & (IN_MOVED_TO | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
      auto wlock = maps.wlock();
      auto old =
          (pending_move*)w_ht_val_ptr(w_ht_get(wlock->move_map, ine->cookie));
      if (old) {
        int wd = inotify_add_watch(infd, name->buf, WATCHMAN_INOTIFY_MASK);
        if (wd == -1) {
          if (errno == ENOSPC || errno == ENOMEM) {
            // Limits exceeded, no recovery from our perspective
            set_poison_state(
                name, now, "inotify-add-watch", errno, inot_strerror(errno));
          } else {
            w_log(
                W_LOG_DBG,
                "add_watch: %s %s\n",
                name->buf,
                inot_strerror(errno));
          }
        } else {
          w_log(W_LOG_DBG, "moved %s -> %s\n", old->name->buf, name->buf);
          w_ht_replace(wlock->wd_to_name, wd, w_ht_ptr_val(name));
        }
      } else {
        w_log(
            W_LOG_DBG,
            "move: cookie=%" PRIx32 " not found in move map %s\n",
            ine->cookie,
            name->buf);
      }
    }

    if (dir_name) {
      if ((ine->mask & (IN_UNMOUNT|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF))) {
        w_string_t *pname;

        if (w_string_equal(root->root_path, name)) {
          w_log(W_LOG_ERR,
              "root dir %s has been (re)moved, canceling watch\n",
              root->root_path.c_str());
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
        pending_flags |= W_PENDING_RECURSIVE;
      }

      if (ine->mask & (IN_CREATE|IN_DELETE)) {
        pending_flags |= W_PENDING_RECURSIVE;
      }

      w_log(W_LOG_DBG, "add_pending for inotify mask=%x %.*s\n",
          ine->mask, name->len, name->buf);
      w_pending_coll_add(coll, name, now, pending_flags);

      w_string_delref(name);

      // The kernel removed the wd -> name mapping, so let's update
      // our state here also
      if ((ine->mask & IN_IGNORED) != 0) {
        w_log(
            W_LOG_DBG,
            "mask=%x: remove watch %d %.*s\n",
            ine->mask,
            ine->wd,
            int(dir_name.size()),
            dir_name.data());
        auto wlock = maps.wlock();
        w_ht_del(wlock->wd_to_name, ine->wd);
      }

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

bool InotifyWatcher::consumeNotify(
    w_root_t* root,
    struct watchman_pending_collection* coll) {
  struct inotify_event *ine;
  char *iptr;
  int n;
  struct timeval now;

  n = read(infd, &ibuf, sizeof(ibuf));
  if (n == -1) {
    if (errno == EINTR) {
      return false;
    }
    w_log(
        W_LOG_FATAL,
        "read(%d, %zu): error %s\n",
        infd,
        sizeof(ibuf),
        strerror(errno));
  }

  w_log(W_LOG_DBG, "inotify read: returned %d.\n", n);
  gettimeofday(&now, nullptr);

  for (iptr = ibuf; iptr < ibuf + n; iptr = iptr + sizeof(*ine) + ine->len) {
    ine = (struct inotify_event*)iptr;

    process_inotify_event(root, coll, ine, now);
  }

  // It is possible that we can accumulate a set of pending_move
  // structs in move_map.  This happens when a directory is moved
  // outside of the watched tree; we get the MOVE_FROM but never
  // get the MOVE_TO with the same cookie.  To avoid leaking these,
  // we'll age out the move_map after processing a full set of
  // inotify events.   We age out rather than delete all because
  // the MOVE_TO may yet be waiting to read in another go around.
  // We allow a somewhat arbitrary but practical grace period to
  // observe the corresponding MOVE_TO.
  {
    auto wlock = maps.wlock();
    if (w_ht_size(wlock->move_map) > 0) {
      w_ht_iter_t iter;
      if (w_ht_first(wlock->move_map, &iter))
        do {
          auto pending = (pending_move*)w_ht_val_ptr(iter.value);
          if (now.tv_sec - pending->created > 5 /* seconds */) {
            w_log(
                W_LOG_DBG,
                "deleting pending move %s (moved outside of watch?)\n",
                pending->name->buf);
            w_ht_iter_del(wlock->move_map, &iter);
          }
        } while (w_ht_next(wlock->move_map, &iter));
    }
  }

  return true;
}

bool InotifyWatcher::waitNotify(int timeoutms) {
  int n;
  struct pollfd pfd;

  pfd.fd = infd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static InotifyWatcher watcher;
Watcher* inotify_watcher = &watcher;

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
