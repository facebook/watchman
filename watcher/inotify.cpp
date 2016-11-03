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
  w_string name;

  pending_move(time_t created, const w_string& name)
      : created(created), name(name) {}
};

struct InotifyWatcher : public Watcher {
  /* we use one inotify instance per watched root dir */
  int infd;
  int terminatePipe_[2]{-1, -1};

  struct maps {
    /* map of active watch descriptor to name of the corresponding dir */
    std::unordered_map<int, w_string> wd_to_name;
    /* map of inotify cookie to corresponding name */
    std::unordered_map<uint32_t, pending_move> move_map;
  };

  watchman::Synchronized<maps> maps;

  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf[WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + 256)];

  InotifyWatcher() : Watcher("inotify", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {}
  ~InotifyWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;

  struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(w_root_t* root, PendingCollection::LockedPtr& coll)
      override;

  bool waitNotify(int timeoutms) override;

  void process_inotify_event(
      w_root_t* root,
      PendingCollection::LockedPtr& coll,
      struct inotify_event* ine,
      struct timeval now);

  void signalThreads() override;
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

  if (pipe(watcher->terminatePipe_)) {
    ignore_result(asprintf(
        errmsg,
        "watch(%s): pipe error: %s",
        root->root_path.c_str(),
        strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(watcher->terminatePipe_[0]);
  w_set_cloexec(watcher->terminatePipe_[1]);

  {
    auto maps = watcher->maps.wlock();
    maps->wd_to_name.reserve(
        root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS));
  }

  root->inner.watcher = std::move(watcher);
  return true;
}

InotifyWatcher::~InotifyWatcher() {
  close(infd);
  if (terminatePipe_[0] != -1) {
    close(terminatePipe_[0]);
  }
  if (terminatePipe_[1] != -1) {
    close(terminatePipe_[1]);
  }
}

struct watchman_dir_handle* InotifyWatcher::startWatchDir(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle* osdir = nullptr;
  int newwd, err;

  // Carry out our very strict opendir first to ensure that we're not
  // traversing symlinks in the context of this root
  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, nullptr);
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
      handle_open_errno(
          root, dir, now, "inotify_add_watch", errno, inot_strerror(errno));
    }
    w_dir_close(osdir);
    errno = err;
    return nullptr;
  }

  // record mapping
  {
    auto wlock = maps.wlock();
    wlock->wd_to_name[newwd] = dir_name;
  }
  w_log(W_LOG_DBG, "adding %d -> %s mapping\n", newwd, path);

  return osdir;
}

void InotifyWatcher::process_inotify_event(
    w_root_t* root,
    PendingCollection::LockedPtr& coll,
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
      auto it = rlock->wd_to_name.find(ine->wd);
      if (it != rlock->wd_to_name.end()) {
        dir_name = it->second;
      }
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

      // record this as a pending move, so that we can automatically
      // watch the target when we get the other side of it.
      {
        auto wlock = maps.wlock();
        wlock->move_map.emplace(ine->cookie, pending_move(now.tv_sec, name));
      }

      w_log(W_LOG_DBG,
          "recording move_from %" PRIx32 " %s\n", ine->cookie,
          name->buf);
    }

    if (ine->len > 0 &&
        (ine->mask & (IN_MOVED_TO | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
      auto wlock = maps.wlock();
      auto it = wlock->move_map.find(ine->cookie);
      if (it != wlock->move_map.end()) {
        auto& old = it->second;
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
          w_log(W_LOG_DBG, "moved %s -> %s\n", old.name.c_str(), name->buf);
          wlock->wd_to_name[wd] = name;
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
      coll->add(name, now, pending_flags);

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
        wlock->wd_to_name.erase(ine->wd);
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
    PendingCollection::LockedPtr& coll) {
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
    if (!wlock->move_map.empty()) {
      auto it = wlock->move_map.begin();
      while (it != wlock->move_map.end()) {
        auto& pending = it->second;
        if (now.tv_sec - pending.created > 5 /* seconds */) {
          w_log(
              W_LOG_DBG,
              "deleting pending move %s (moved outside of watch?)\n",
              pending.name.c_str());
          it = wlock->move_map.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  return true;
}

bool InotifyWatcher::waitNotify(int timeoutms) {
  int n;
  std::array<struct pollfd, 2> pfd;

  pfd[0].fd = infd;
  pfd[0].events = POLLIN;
  pfd[1].fd = terminatePipe_[0];
  pfd[1].events = POLLIN;

  n = poll(pfd.data(), pfd.size(), timeoutms);

  if (n > 0) {
    if (pfd[1].revents) {
      // We were signalled via signalThreads
      return false;
    }
    return pfd[0].revents != 0;
  }
  return false;
}

void InotifyWatcher::signalThreads() {
  ignore_result(write(terminatePipe_[1], "X", 1));
}

static InotifyWatcher watcher;
Watcher* inotify_watcher = &watcher;

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
