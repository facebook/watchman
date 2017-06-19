/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
#include "watchman_synchronized.h"

#include <algorithm>
#include <condition_variable>
#include <list>
#include <iterator>
#include <tuple>
#include <mutex>

#ifdef _WIN32

#define NETWORK_BUF_SIZE (64*1024)

namespace {

struct Item {
  w_string path;
  int flags;

  Item(w_string &&path, int flags) : path(std::move(path)), flags(flags) {}
};

}

struct WinWatcher : public Watcher {
  HANDLE ping{INVALID_HANDLE_VALUE}, olapEvent{INVALID_HANDLE_VALUE};
  HANDLE dir_handle{INVALID_HANDLE_VALUE};

  std::condition_variable cond;
  watchman::Synchronized<std::list<Item>, std::mutex> changedItems;

  explicit WinWatcher(w_root_t* root);
  ~WinWatcher();

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  bool start(const std::shared_ptr<w_root_t>& root) override;
  void signalThreads() override;
  void readChangesThread(const std::shared_ptr<w_root_t>& root);
};

WinWatcher::WinWatcher(w_root_t* root)
    : Watcher("win32", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
  int err;

  auto wpath = root->root_path.piece().asWideUNC();

  // Create an overlapped handle so that we can avoid blocking forever
  // in ReadDirectoryChangesW
  dir_handle = CreateFileW(
      wpath.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (!dir_handle) {
    throw std::runtime_error(
        std::string("failed to open dir ") + root->root_path.c_str() + ": " +
        win32_strerror(GetLastError()));
  }

  ping = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!ping) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
  olapEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!olapEvent) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
}

WinWatcher::~WinWatcher() {
  if (ping != INVALID_HANDLE_VALUE) {
    CloseHandle(ping);
  }
  if (olapEvent != INVALID_HANDLE_VALUE) {
    CloseHandle(olapEvent);
  }
  if (dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(dir_handle);
  }
}

void WinWatcher::signalThreads() {
  SetEvent(ping);
}

void WinWatcher::readChangesThread(const std::shared_ptr<w_root_t>& root) {
  DWORD size = WATCHMAN_BATCH_LIMIT * (sizeof(FILE_NOTIFY_INFORMATION) + 512);
  std::vector<uint8_t> buf;
  DWORD err, filter;
  OVERLAPPED olap;
  BOOL initiate_read = true;
  HANDLE handles[2] = {olapEvent, ping};
  DWORD bytes;

  w_set_thread_name("readchange %s", root->root_path.c_str());
  watchman::log(watchman::DBG, "initializing\n");

  // Artificial extra latency to impose around processing changes.
  // This is needed to avoid trying to access files and dirs too
  // soon after a change is noticed, as this can cause recursive
  // deletes to fail.
  auto extraLatency = root->config.getInt("win32_batch_latency_ms", 30);

  // Block until winmatch_root_st is waiting for our initialization
  {
    auto wlock = changedItems.wlock();

    filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE;

    memset(&olap, 0, sizeof(olap));
    olap.hEvent = olapEvent;

    buf.resize(size);

    if (!ReadDirectoryChangesW(
            dir_handle, &buf[0], size, TRUE, filter, nullptr, &olap, nullptr)) {
      err = GetLastError();
      w_log(
          W_LOG_ERR,
          "ReadDirectoryChangesW: failed, cancel watch. %s\n",
          win32_strerror(err));
      root->cancel();
      return;
    }
    // Signal that we are done with init.  We MUST do this AFTER our first
    // successful ReadDirectoryChangesW, otherwise there is a race condition
    // where we'll miss observing the cookie for a query that comes in
    // after we've crawled but before the watch is established.
    w_log(W_LOG_DBG, "ReadDirectoryChangesW signalling as init done\n");
    cond.notify_one();
  }
  initiate_read = false;

  std::list<Item> items;

  // The mutex must not be held when we enter the loop
  while (!root->inner.cancelled) {
    if (initiate_read) {
      if (!ReadDirectoryChangesW(
              dir_handle,
              &buf[0],
              size,
              TRUE,
              filter,
              nullptr,
              &olap,
              nullptr)) {
        err = GetLastError();
        w_log(W_LOG_ERR,
            "ReadDirectoryChangesW: failed, cancel watch. %s\n",
            win32_strerror(err));
        root->cancel();
        break;
      } else {
        initiate_read = false;
      }
    }

    watchman::log(watchman::DBG, "waiting for change notifications\n");
    DWORD status = WaitForMultipleObjects(
        2,
        handles,
        FALSE,
        // We use a 10 second timeout by default until we start accumulating a
        // batch.  Once we have a batch we prefer to add more to it than notify
        // immediately, so we introduce a 30ms latency.  Without this artificial
        // latency we'll wake up and start trying to look at a directory that
        // may be in the process of being recursively deleted and that act can
        // block the recursive delete.
        items.empty() ? 10000 : extraLatency);
    watchman::log(watchman::DBG, "wait returned with status ", status, "\n");

    if (status == WAIT_OBJECT_0) {
      bytes = 0;
      if (!GetOverlappedResult(dir_handle, &olap, &bytes, FALSE)) {
        err = GetLastError();
        w_log(
            W_LOG_ERR,
            "overlapped ReadDirectoryChangesW(%s): 0x%x %s\n",
            root->root_path.c_str(),
            err,
            win32_strerror(err));

        if (err == ERROR_INVALID_PARAMETER && size > NETWORK_BUF_SIZE) {
          // May be a network buffer related size issue; the docs say that
          // we can hit this when watching a UNC path. Let's downsize and
          // retry the read just one time
          w_log(
              W_LOG_ERR,
              "retrying watch for possible network location %s "
              "with smaller buffer\n",
              root->root_path.c_str());
          size = NETWORK_BUF_SIZE;
          initiate_read = true;
          continue;
        }

        if (err == ERROR_NOTIFY_ENUM_DIR) {
          root->scheduleRecrawl("ERROR_NOTIFY_ENUM_DIR");
        } else {
          w_log(
              W_LOG_ERR, "Cancelling watch for %s\n", root->root_path.c_str());
          root->cancel();
          break;
        }
      } else {
        PFILE_NOTIFY_INFORMATION not = (PFILE_NOTIFY_INFORMATION)buf.data();

        while (true) {
          DWORD n_chars;

          // FileNameLength is in BYTES, but FileName is WCHAR
          n_chars = not->FileNameLength / sizeof(not->FileName[0]);
          w_string name(not->FileName, n_chars);

          auto full = w_string::pathCat({root->root_path, name});

          if (!root->ignore.isIgnored(full.data(), full.size())) {
            // If we have a delete or rename-away it may be part of
            // a recursive tree remove or rename.  In that situation
            // the notifications that we'll receive from the OS will
            // be from the leaves and bubble up to the root of the
            // delete/rename.  We want to flag those paths for recursive
            // analysis so that we can prune children from the trie
            // that is built when we pass this to the pending list
            // later.  We don't do that here in this thread because
            // we're trying to minimize latency in this context.
            items.emplace_back(
                std::move(full),
                (not->Action &
                 (FILE_ACTION_REMOVED | FILE_ACTION_RENAMED_OLD_NAME)) != 0
                    ? W_PENDING_RECURSIVE
                    : 0);
          }

          // Advance to next item
          if (not->NextEntryOffset == 0) {
            break;
          }
          not = (PFILE_NOTIFY_INFORMATION)(not->NextEntryOffset + (char*)not);
        }

        ResetEvent(olapEvent);
        initiate_read = true;
      }
    } else if (status == WAIT_OBJECT_0 + 1) {
      w_log(W_LOG_ERR, "signalled\n");
      break;
    } else if (status == WAIT_TIMEOUT) {
      if (!items.empty()) {
        watchman::log(watchman::DBG,
                      "timed out waiting for changes, and we have ",
                      items.size(), " items; move and notify\n");
        auto wlock = changedItems.wlock();
        wlock->splice(wlock->end(), items);
        cond.notify_one();
      }
    } else {
      w_log(W_LOG_ERR, "impossible wait status=%d\n", status);
      break;
    }
  }

  w_log(W_LOG_DBG, "done\n");
}

bool WinWatcher::start(const std::shared_ptr<w_root_t>& root) {
  int err;

  // Spin up the changes reading thread; it owns a ref on the root

  try {
    // Acquire the mutex so thread initialization waits until we release it
    auto wlock = changedItems.wlock();

    watchman::log(watchman::DBG, "starting readChangesThread\n");
    auto self = std::dynamic_pointer_cast<WinWatcher>(shared_from_this());
    std::thread thread([self, root]() {
      try {
        self->readChangesThread(root);
      } catch (const std::exception& e) {
        watchman::log(watchman::ERR, "uncaught exception: ", e.what());
        root->cancel();
      }

      // Ensure that we signal the condition variable before we
      // finish this thread.  That ensures that don't get stuck
      // waiting in WinWatcher::start if something unexpected happens.
      auto wlock = self->changedItems.wlock();
      self->cond.notify_one();
    });
    // We have to detach because the readChangesThread may wind up
    // being the last thread to reference the watcher state and
    // cannot join itself.
    thread.detach();

    // Allow thread init to proceed; wait for its signal
    if (cond.wait_for(wlock.getUniqueLock(), std::chrono::seconds(10)) ==
        std::cv_status::timeout) {
      watchman::log(
          watchman::ERR, "timedout waiting for readChangesThread to start\n");
      root->cancel();
      return false;
    }

    if (root->failure_reason) {
      w_log(
          W_LOG_ERR,
          "failed to start readchanges thread: %s\n",
          root->failure_reason.c_str());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    w_log(W_LOG_ERR, "failed to start readchanges thread: %s\n", e.what());
    return false;
  }
}

std::unique_ptr<watchman_dir_handle> WinWatcher::startWatchDir(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  return w_dir_open(path);
}

bool WinWatcher::consumeNotify(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& coll) {
  std::list<Item> items;
  struct timeval now;

  {
    auto wlock = changedItems.wlock();
    std::swap(items, *wlock);
  }

  gettimeofday(&now, nullptr);

  for (auto& item : items) {
    watchman::log(watchman::DBG, "readchanges: add pending ", item.path, " ",
                  item.flags, "\n");
    coll->add(item.path, now, W_PENDING_VIA_NOTIFY | item.flags);
  }

  return !items.empty();
}

bool WinWatcher::waitNotify(int timeoutms) {
  auto wlock = changedItems.wlock();
  cond.wait_for(wlock.getUniqueLock(), std::chrono::milliseconds(timeoutms));
  return !wlock->empty();
}

static RegisterWatcher<WinWatcher> reg("win32");

#endif // _WIN32

/* vim:ts=2:sw=2:et:
 */
