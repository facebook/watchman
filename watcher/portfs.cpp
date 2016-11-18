/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "make_unique.h"
#include "watchman.h"
#include "InMemoryView.h"

#ifdef HAVE_PORT_CREATE

#define WATCHMAN_PORT_EVENTS \
  FILE_MODIFIED | FILE_ATTRIB | FILE_NOFOLLOW

struct watchman_port_file {
  file_obj_t port_file;
  w_string name;
};

struct PortFSWatcher : public Watcher {
  int port_fd;
  /* map of file name to watchman_port_file */
  watchman::Synchronized<
      std::unordered_map<w_string, std::unique_ptr<watchman_port_file>>>
      port_files;
  port_event_t portevents[WATCHMAN_BATCH_LIMIT];

  explicit PortFSWatcher(w_root_t* root);
  ~PortFSWatcher();

  struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  bool consumeNotify(w_root_t* root, struct watchman_pending_collection* coll)
      override;

  bool waitNotify(int timeoutms) override;
  bool do_watch(w_string_t* name, struct stat* st);
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
    {0, nullptr},
};

static std::unique_ptr<watchman_port_file> make_port_file(
    const w_string& name,
    struct stat* st) {
  auto f = watchman::make_unique<watchman_port_file>();

  f->name = name;
  f->port_file.fo_name = (char*)name->buf;
  f->port_file.fo_atime = st->st_atim;
  f->port_file.fo_mtime = st->st_mtim;
  f->port_file.fo_ctime = st->st_ctim;

  return f;
}

PortFSWatcher::PortFSWatcher(w_root_t* root) : Watcher("portfs", 0) {
  port_files.reserve(root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS));

  port_fd = port_create();
  if (port_fd == -1) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string("port_create() error: ") + strerror(errno));
  }
  w_set_cloexec(port_fd);
}

PortFSWatcher::~PortFSWatcher() {
  close(port_fd);
  port_fd = -1;
}

bool PortFSWatcher::do_watch(w_string_t* name, struct stat* st) {
  bool success = false;

  auto wlock = port_files.wlock();
  if (port_files.find(name) != port_files.end()) {
    // Already watching it
    return true;
  }

  auto f = make_port_file(name, st);
  auto rawFile = f.get();
  wlock->emplace(name, std::move(f));

  w_log(W_LOG_DBG, "watching %s\n", name->buf);
  errno = 0;
  if (port_associate(
          port_fd,
          PORT_SOURCE_FILE,
          (uintptr_t)&rawFile->port_file,
          WATCHMAN_PORT_EVENTS,
          (void*)rawFile)) {
    w_log(
        W_LOG_ERR,
        "port_associate %s %s\n",
        rawFile->port_file.fo_name,
        strerror(errno));
    wlock->erase(name);
    return false;
  }

  return true;
}

bool PortFSWatcher::startWatchFile(struct watchman_file* file) {
  w_string_t *name;
  bool success = false;

  name = w_string_path_cat(file->parent->path, file->name);
  if (!name) {
    return false;
  }
  success = do_watch(name, &file->st);
  w_string_delref(name);

  return success;
}

struct watchman_dir_handle* PortFSWatcher::startWatchDir(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;
  struct stat st;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  if (fstat(dirfd(osdir), &st) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    root->scheduleRecrawl("fstat failed");
    w_dir_close(osdir);
    return nullptr;
  }

  auto dir_name = dir->getFullPath();
  if (!do_watch(dir_name, &st)) {
    w_dir_close(osdir);
    w_string_delref(dir_name);
    return nullptr;
  }

  w_string_delref(dir_name);
  return osdir;
}

bool PortFSWatcher::consumeNotify(
    w_root_t* root,
    struct watchman_pending_collection* coll) {
  uint_t i, n;
  struct timeval now;

  errno = 0;

  n = 1;
  if (port_getn(
          port_fd,
          portevents,
          sizeof(portevents) / sizeof(portevents[0]),
          &n,
          nullptr)) {
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

  auto wlock = port_files.wlock();

  for (i = 0; i < n; i++) {
    struct watchman_port_file *f;
    uint32_t pe = portevents[i].portev_events;
    char flags_label[128];

    f = (struct watchman_port_file*)portevents[i].portev_user;
    w_expand_flags(pflags, pe, flags_label, sizeof(flags_label));
    w_log(W_LOG_DBG, "port: %s [0x%x %s]\n",
        f->port_file.fo_name,
        pe, flags_label);

    if ((pe & (FILE_RENAME_FROM|UNMOUNTED|MOUNTEDOVER|FILE_DELETE))
        && w_string_equal(f->name, root->root_path)) {
      w_log(
          W_LOG_ERR,
          "root dir %s has been (re)moved (code 0x%x %s), canceling watch\n",
          root->root_path.c_str(),
          pe,
          flags_label);

      root->cancel();
      return false;
    }
    w_pending_coll_add(coll, f->name, now,
        W_PENDING_RECURSIVE|W_PENDING_VIA_NOTIFY);

    // It was port_dissociate'd implicitly.  We'll re-establish a
    // watch later when portfs_root_start_watch_(file|dir) are called again
    wlock->erase(f->name);
  }

  return true;
}

bool PortFSWatcher::waitNotify(int timeoutms) {
  int n;
  struct pollfd pfd;

  pfd.fd = port_fd;
  pfd.events = POLLIN;

  n = poll(&pfd, 1, timeoutms);

  return n == 1;
}

static RegisterWatcher<PortFSWatcher> reg(
    "portfs",
    1 /* higher priority than inotify */);

#endif // HAVE_PORT_CREATE

/* vim:ts=2:sw=2:et:
 */
