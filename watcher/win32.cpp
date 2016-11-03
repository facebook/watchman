/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

#ifdef _WIN32

#define NETWORK_BUF_SIZE (64*1024)

struct winwatch_changed_item {
  struct winwatch_changed_item* next{nullptr};
  w_string name;
};

struct WinWatcher : public Watcher {
  HANDLE ping{INVALID_HANDLE_VALUE}, olap{INVALID_HANDLE_VALUE};
  HANDLE dir_handle{INVALID_HANDLE_VALUE};

  pthread_mutex_t mtx;
  pthread_cond_t cond;
  pthread_t thread;
  bool thread_started{false};

  struct winwatch_changed_item *head{nullptr}, *tail{nullptr};

  WinWatcher() : Watcher("win32", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {}
  ~WinWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;

  struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(w_root_t* root, PendingCollection::LockedPtr& coll)
      override;

  bool waitNotify(int timeoutms) override;
  bool start(w_root_t* root) override;
  void signalThreads() override;
};

bool WinWatcher::initNew(w_root_t* root, char** errmsg) {
  auto watcher = watchman::make_unique<WinWatcher>();
  WCHAR *wpath;
  int err;

  if (!watcher) {
    *errmsg = strdup("out of memory");
    return false;
  }

  wpath = w_utf8_to_win_unc(root->root_path.data(), root->root_path.size());
  if (!wpath) {
    asprintf(errmsg, "failed to convert root path to WCHAR: %s",
        win32_strerror(GetLastError()));
    return false;
  }

  // Create an overlapped handle so that we can avoid blocking forever
  // in ReadDirectoryChangesW
  watcher->dir_handle = CreateFileW(
      wpath,
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (!watcher->dir_handle) {
    asprintf(
        errmsg,
        "failed to open dir %s: %s",
        root->root_path.c_str(),
        win32_strerror(GetLastError()));
    return false;
  }

  watcher->ping = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!watcher->ping) {
    asprintf(errmsg, "failed to create event: %s",
        win32_strerror(GetLastError()));
    return false;
  }
  watcher->olap = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!watcher->olap) {
    asprintf(errmsg, "failed to create event: %s",
        win32_strerror(GetLastError()));
    return false;
  }
  err = pthread_mutex_init(&watcher->mtx, nullptr);
  if (err) {
    asprintf(errmsg, "failed to init mutex: %s",
        strerror(err));
    return false;
  }
  err = pthread_cond_init(&watcher->cond, nullptr);
  if (err) {
    asprintf(errmsg, "failed to init cond: %s",
        strerror(err));
    return false;
  }

  root->inner.watcher = std::move(watcher);
  return true;
}

WinWatcher::~WinWatcher() {
  // wait for readchanges_thread to quit before we tear down state
  if (thread_started && !pthread_equal(thread, pthread_self())) {
    void *ignore;
    pthread_join(thread, &ignore);
  }

  pthread_mutex_destroy(&mtx);
  if (ping != INVALID_HANDLE_VALUE) {
    CloseHandle(ping);
  }
  if (olap != INVALID_HANDLE_VALUE) {
    CloseHandle(olap);
  }
  if (dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(dir_handle);
  }
}

void WinWatcher::signalThreads() {
  SetEvent(ping);
}

static void *readchanges_thread(void *arg) {
  w_root_t *root = (w_root_t*)arg;
  auto watcher = (WinWatcher*)root->inner.watcher.get();
  DWORD size = WATCHMAN_BATCH_LIMIT * (sizeof(FILE_NOTIFY_INFORMATION) + 512);
  char *buf;
  DWORD err, filter;
  OVERLAPPED olap;
  BOOL initiate_read = true;
  HANDLE handles[2] = {watcher->olap, watcher->ping};
  DWORD bytes;

  w_set_thread_name("readchange %s", root->root_path.c_str());

  // Block until winmatch_root_st is waiting for our initialization
  pthread_mutex_lock(&watcher->mtx);

  filter = FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|
    FILE_NOTIFY_CHANGE_ATTRIBUTES|FILE_NOTIFY_CHANGE_SIZE|
    FILE_NOTIFY_CHANGE_LAST_WRITE;

  memset(&olap, 0, sizeof(olap));
  olap.hEvent = watcher->olap;

  buf = (char*)malloc(size);
  if (!buf) {
    w_log(W_LOG_ERR, "failed to allocate %u bytes for dirchanges buf\n", size);
    goto out;
  }

  if (!ReadDirectoryChangesW(
          watcher->dir_handle,
          buf,
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
    w_root_cancel(root);
    goto out;
  }
  // Signal that we are done with init.  We MUST do this AFTER our first
  // successful ReadDirectoryChangesW, otherwise there is a race condition
  // where we'll miss observing the cookie for a query that comes in
  // after we've crawled but before the watch is established.
  w_log(W_LOG_DBG, "ReadDirectoryChangesW signalling as init done");
  pthread_cond_signal(&watcher->cond);
  pthread_mutex_unlock(&watcher->mtx);
  initiate_read = false;

  // The watcher->mutex must not be held when we enter the loop
  while (!root->inner.cancelled) {
    if (initiate_read) {
      if (!ReadDirectoryChangesW(
              watcher->dir_handle,
              buf,
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
        w_root_cancel(root);
        break;
      } else {
        initiate_read = false;
      }
    }

    w_log(W_LOG_DBG, "waiting for change notifications");
    DWORD status = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    if (status == WAIT_OBJECT_0) {
      bytes = 0;
      if (!GetOverlappedResult(watcher->dir_handle, &olap, &bytes, FALSE)) {
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
          w_root_schedule_recrawl(root, "ERROR_NOTIFY_ENUM_DIR");
        } else {
          w_log(
              W_LOG_ERR, "Cancelling watch for %s\n", root->root_path.c_str());
          w_root_cancel(root);
          break;
        }
      } else {
        PFILE_NOTIFY_INFORMATION not = (PFILE_NOTIFY_INFORMATION)buf;
        struct winwatch_changed_item *head = nullptr, *tail = nullptr;
        while (true) {
          struct winwatch_changed_item *item;
          DWORD n_chars;
          w_string_t* name;

          // FileNameLength is in BYTES, but FileName is WCHAR
          n_chars = not->FileNameLength / sizeof(not->FileName[0]);
          name = w_string_new_wchar_typed(not->FileName, n_chars, W_STRING_BYTE);

          auto full = w_string::pathCat({root->root_path, name});
          w_string_delref(name);

          if (!root->ignore.isIgnored(full.data(), full.size())) {
            item = new winwatch_changed_item;
            item->name = full;

            if (tail) {
              tail->next = item;
            } else {
              head = item;
            }
            tail = item;
          }

          // Advance to next item
          if (not->NextEntryOffset == 0) {
            break;
          }
          not = (PFILE_NOTIFY_INFORMATION)(not->NextEntryOffset + (char*)not);
        }

        if (tail) {
          pthread_mutex_lock(&watcher->mtx);
          if (watcher->tail) {
            watcher->tail->next = head;
          } else {
            watcher->head = head;
          }
          watcher->tail = tail;
          pthread_mutex_unlock(&watcher->mtx);
          pthread_cond_signal(&watcher->cond);
        }
        ResetEvent(watcher->olap);
        initiate_read = true;
      }
    } else if (status == WAIT_OBJECT_0 + 1) {
      w_log(W_LOG_ERR, "signalled\n");
      break;
    } else {
      w_log(W_LOG_ERR, "impossible wait status=%d\n",
          status);
      break;
    }
  }

  pthread_mutex_lock(&watcher->mtx);
out:
  // Signal to winwatch_root_start that we're done initializing in
  // the failure path.  We'll also do this after we've completed
  // the run loop in the success path; it's a spurious wakeup but
  // harmless and saves us from adding and setting a control flag
  // in each of the failure `goto` statements. winwatch_root_dtor
  // will `pthread_join` us before `watcher` is freed.
  pthread_cond_signal(&watcher->cond);
  pthread_mutex_unlock(&watcher->mtx);

  if (buf) {
    free(buf);
  }
  w_log(W_LOG_DBG, "done\n");
  w_root_delref_raw(root);
  return nullptr;
}

bool WinWatcher::start(w_root_t* root) {
  int err;
  unused_parameter(root);

  // Spin up the changes reading thread; it owns a ref on the root
  w_root_addref(root);

  // Acquire the mutex so thread initialization waits until we release it
  pthread_mutex_lock(&mtx);

  err = pthread_create(&thread, nullptr, readchanges_thread, root);
  if (err == 0) {
    // Allow thread init to proceed; wait for its signal
    pthread_cond_wait(&cond, &mtx);
    pthread_mutex_unlock(&mtx);

    if (root->failure_reason) {
      w_log(
          W_LOG_ERR,
          "failed to start readchanges thread: %s\n",
          root->failure_reason.c_str());
      return false;
    }
    thread_started = true;
    return true;
  }

  pthread_mutex_unlock(&mtx);
  w_root_delref_raw(root);
  w_log(W_LOG_ERR, "failed to start readchanges thread: %s\n", strerror(err));
  return false;
}

struct watchman_dir_handle* WinWatcher::startWatchDir(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, strerror(errno));
    return nullptr;
  }

  return osdir;
}

bool WinWatcher::consumeNotify(
    w_root_t* root,
    PendingCollection::LockedPtr& coll) {
  struct winwatch_changed_item *list, *item;
  struct timeval now;
  int n = 0;

  pthread_mutex_lock(&mtx);
  list = head;
  head = nullptr;
  tail = nullptr;
  pthread_mutex_unlock(&mtx);

  gettimeofday(&now, nullptr);

  while (list) {
    item = list;
    list = item->next;
    n++;

    w_log(
        W_LOG_DBG,
        "readchanges: add pending %.*s\n",
        int(item->name.size()),
        item->name.data());
    coll->add(item->name, now, W_PENDING_VIA_NOTIFY);

    delete item;
  }

  return n > 0;
}

bool WinWatcher::waitNotify(int timeoutms) {
  struct timeval now, delta, target;
  struct timespec ts;

  if (timeoutms == 0 || head) {
    return head ? true : false;
  }

  // Add timeout to current time, convert to absolute timespec
  gettimeofday(&now, nullptr);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, &ts);

  pthread_mutex_lock(&mtx);
  pthread_cond_timedwait(&cond, &mtx, &ts);
  pthread_mutex_unlock(&mtx);
  return head ? true : false;
}

static WinWatcher watcher;
Watcher* win32_watcher = &watcher;

#endif // _WIN32

/* vim:ts=2:sw=2:et:
 */
