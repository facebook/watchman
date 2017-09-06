/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "InMemoryView.h"
#include "make_unique.h"
#include "watchman.h"

#ifdef HAVE_PORT_CREATE

#define WATCHMAN_PORT_EVENTS FILE_MODIFIED | FILE_ATTRIB | FILE_NOFOLLOW

struct watchman_port_file {
  file_obj_t port_file;
  w_string name;
};

using watchman::FileDescriptor;
using watchman::Pipe;

struct PortFSWatcher : public Watcher {
  FileDescriptor port_fd;
  Pipe terminatePipe_;

  /* map of file name to watchman_port_file */
  watchman::Synchronized<
      std::unordered_map<w_string, std::unique_ptr<watchman_port_file>>>
      port_files;

  port_event_t portevents[WATCHMAN_BATCH_LIMIT];

  explicit PortFSWatcher(w_root_t* root);

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  bool consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
  bool do_watch(const w_string& name, const watchman::FileInformation& finfo);
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
    const watchman::FileInformation& finfo) {
  auto f = watchman::make_unique<watchman_port_file>();

  f->name = name;
  f->port_file.fo_name = (char*)name.c_str();
  f->port_file.fo_atime = finfo.atime;
  f->port_file.fo_mtime = finfo.mtime;
  f->port_file.fo_ctime = finfo.ctime;

  return f;
}

PortFSWatcher::PortFSWatcher(w_root_t* root)
    : Watcher("portfs", 0), port_fd(port_create(), "port_create()") {
  auto wlock = port_files.wlock();
  wlock->reserve(root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS));
  port_fd.setCloExec();
}

bool PortFSWatcher::do_watch(
    const w_string& name,
    const watchman::FileInformation& finfo) {
  auto wlock = port_files.wlock();
  if (wlock->find(name) != wlock->end()) {
    // Already watching it
    return true;
  }

  auto f = make_port_file(name, finfo);
  auto rawFile = f.get();
  wlock->emplace(name, std::move(f));

  w_log(W_LOG_DBG, "watching %s\n", name.c_str());
  errno = 0;
  if (port_associate(
          port_fd.fd(),
          PORT_SOURCE_FILE,
          (uintptr_t)&rawFile->port_file,
          WATCHMAN_PORT_EVENTS,
          (void*)rawFile)) {
    int err = errno;
    w_log(
        W_LOG_ERR,
        "port_associate %s %s\n",
        rawFile->port_file.fo_name,
        strerror(errno));
    wlock->erase(name);
    throw std::system_error(err, std::generic_category(), "port_associate");
    return false;
  }

  return true;
}

bool PortFSWatcher::startWatchFile(struct watchman_file* file) {
  w_string name;
  bool success = false;

  name = w_dir_path_cat_str(file->parent, file->getName());
  if (!name) {
    return false;
  }
  success = do_watch(name, file->stat);
  w_string_delref(name);

  return success;
}

std::unique_ptr<watchman_dir_handle> PortFSWatcher::startWatchDir(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_dir* dir,
    struct timeval,
    const char* path) {
  struct stat st;

  auto osdir = w_dir_open(path);

  if (fstat(osdir->getFd(), &st) == -1) {
    // whaaa?
    root->scheduleRecrawl("fstat failed");
    throw std::system_error(
        errno,
        std::generic_category(),
        std::string("fstat failed for dir ") + path);
  }

  auto dir_name = dir->getFullPath();
  if (!do_watch(dir_name, watchman::FileInformation(st))) {
    return nullptr;
  }

  return osdir;
}

bool PortFSWatcher::consumeNotify(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& coll) {
  uint_t i, n;
  struct timeval now;

  errno = 0;

  n = 1;
  if (port_getn(
          port_fd.fd(),
          portevents,
          sizeof(portevents) / sizeof(portevents[0]),
          &n,
          nullptr)) {
    if (errno == EINTR) {
      return false;
    }
    w_log(W_LOG_FATAL, "port_getn: %s\n", strerror(errno));
  }

  w_log(W_LOG_DBG, "port_getn: n=%u\n", n);

  if (n == 0) {
    return false;
  }

  auto wlock = port_files.wlock();

  gettimeofday(&now, nullptr);
  for (i = 0; i < n; i++) {
    struct watchman_port_file* f;
    uint32_t pe = portevents[i].portev_events;
    char flags_label[128];

    f = (struct watchman_port_file*)portevents[i].portev_user;
    w_expand_flags(pflags, pe, flags_label, sizeof(flags_label));
    w_log(
        W_LOG_DBG,
        "port: %s [0x%x %s]\n",
        f->port_file.fo_name,
        pe,
        flags_label);

    if ((pe & (FILE_RENAME_FROM | UNMOUNTED | MOUNTEDOVER | FILE_DELETE)) &&
        w_string_equal(f->name, root->root_path)) {
      w_log(
          W_LOG_ERR,
          "root dir %s has been (re)moved (code 0x%x %s), canceling watch\n",
          root->root_path.c_str(),
          pe,
          flags_label);

      root->cancel();
      return false;
    }
    coll->add(f->name, now, W_PENDING_RECURSIVE | W_PENDING_VIA_NOTIFY);

    // It was port_dissociate'd implicitly.  We'll re-establish a
    // watch later when portfs_root_start_watch_(file|dir) are called again
    wlock->erase(f->name);
  }

  return true;
}

bool PortFSWatcher::waitNotify(int timeoutms) {
  int n;
  std::array<struct pollfd, 2> pfd;

  pfd[0].fd = port_fd.fd();
  pfd[0].events = POLLIN;
  pfd[1].fd = terminatePipe_.read.fd();
  pfd[1].events = POLLIN;

  n = poll(pfd.data(), pfd.size(), timeoutms);

  if (n > 0) {
    if (pfd[1].revents) {
      // We were signalled via signalThreads
      return false;
    }
    return pfd[0].revents != 0;
  }
  return n == 1;
}

void PortFSWatcher::signalThreads() {
  ignore_result(write(terminatePipe_.write.fd(), "X", 1));
}

static RegisterWatcher<PortFSWatcher> reg(
    "portfs",
    1 /* higher priority than inotify */);

#endif // HAVE_PORT_CREATE

/* vim:ts=2:sw=2:et:
 */
