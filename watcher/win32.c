/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef _WIN32

#define NETWORK_BUF_SIZE (64*1024)

struct winwatch_changed_item {
  struct winwatch_changed_item *next;
  w_string_t *name;
};

struct winwatch_root_state {
  HANDLE ping, olap;
  HANDLE dir_handle;

  pthread_mutex_t mtx;
  pthread_cond_t cond;
  pthread_t thread;

  struct winwatch_changed_item *head, *tail;
};

bool winwatch_root_init(w_root_t *root, char **errmsg) {
  struct winwatch_root_state *state;
  WCHAR *wpath;
  int err;


  state = calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

  wpath = w_utf8_to_win_unc(root->root_path->buf, root->root_path->len);
  if (!wpath) {
    asprintf(errmsg, "failed to convert root path to WCHAR: %s",
        win32_strerror(GetLastError()));
    return false;
  }

  // Create an overlapped handle so that we can avoid blocking forever
  // in ReadDirectoryChangesW
  state->dir_handle = CreateFileW(wpath, GENERIC_READ,
      FILE_SHARE_READ|FILE_SHARE_DELETE|FILE_SHARE_WRITE,
      NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED, NULL);
  if (!state->dir_handle) {
    asprintf(errmsg, "failed to open dir %s: %s",
        root->root_path->buf, win32_strerror(GetLastError()));
    return false;
  }

  state->ping = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!state->ping) {
    asprintf(errmsg, "failed to create event: %s",
        win32_strerror(GetLastError()));
    return false;
  }
  state->olap = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!state->olap) {
    asprintf(errmsg, "failed to create event: %s",
        win32_strerror(GetLastError()));
    return false;
  }
  err = pthread_mutex_init(&state->mtx, NULL);
  if (err) {
    asprintf(errmsg, "failed to init mutex: %s",
        strerror(err));
    return false;
  }
  err = pthread_cond_init(&state->cond, NULL);
  if (err) {
    asprintf(errmsg, "failed to init cond: %s",
        strerror(err));
    return false;
  }

  return true;
}

void winwatch_root_dtor(w_root_t *root) {
  struct winwatch_root_state *state = root->watch;

  if (!state) {
    return;
  }

  // wait for readchanges_thread to quit before we tear down state
  if (!pthread_equal(state->thread, pthread_self())) {
    void *ignore;
    pthread_join(state->thread, &ignore);
  }

  pthread_mutex_destroy(&state->mtx);
  CloseHandle(state->ping);
  CloseHandle(state->olap);
  CloseHandle(state->dir_handle);

  free(state);
  root->watch = NULL;
}

static void winwatch_root_signal_threads(w_root_t *root) {
  struct winwatch_root_state *state = root->watch;

  SetEvent(state->ping);
}

static void *readchanges_thread(void *arg) {
  w_root_t *root = arg;
  struct winwatch_root_state *state = root->watch;
  DWORD size = WATCHMAN_BATCH_LIMIT * (sizeof(FILE_NOTIFY_INFORMATION) + 512);
  char *buf;
  DWORD err, filter;
  OVERLAPPED olap;
  BOOL initiate_read = true;
  HANDLE handles[2] = { state->olap, state->ping };
  DWORD bytes;

  w_set_thread_name("readchange %.*s",
      root->root_path->len, root->root_path->buf);

  // Block until winmatch_root_st is waiting for our initialization
  pthread_mutex_lock(&state->mtx);

  filter = FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|
    FILE_NOTIFY_CHANGE_ATTRIBUTES|FILE_NOTIFY_CHANGE_SIZE|
    FILE_NOTIFY_CHANGE_LAST_WRITE;

  memset(&olap, 0, sizeof(olap));
  olap.hEvent = state->olap;

  buf = malloc(size);
  if (!buf) {
    w_log(W_LOG_ERR, "failed to allocate %u bytes for dirchanges buf\n", size);
    goto out;
  }

  if (!ReadDirectoryChangesW(state->dir_handle, buf, size,
        TRUE, filter, NULL, &olap, NULL)) {
    err = GetLastError();
    w_log(W_LOG_ERR,
        "ReadDirectoryChangesW: failed, cancel watch. %s\n",
        win32_strerror(err));
    w_root_lock(root, "w_root_cancel");
    w_root_cancel(root);
    w_root_unlock(root);
    goto out;
  }
  // Signal that we are done with init.  We MUST do this AFTER our first
  // successful ReadDirectoryChangesW, otherwise there is a race condition
  // where we'll miss observing the cookie for a query that comes in
  // after we've crawled but before the watch is established.
  w_log(W_LOG_DBG, "ReadDirectoryChangesW signalling as init done");
  pthread_cond_signal(&state->cond);
  pthread_mutex_unlock(&state->mtx);
  initiate_read = false;

  // The state->mutex must not be held when we enter the loop
  while (!root->cancelled) {
    if (initiate_read) {
      if (!ReadDirectoryChangesW(state->dir_handle,
            buf, size, TRUE, filter, NULL, &olap, NULL)) {
        err = GetLastError();
        w_log(W_LOG_ERR,
            "ReadDirectoryChangesW: failed, cancel watch. %s\n",
            win32_strerror(err));
        w_root_lock(root, "w_root_cancel");
        w_root_cancel(root);
        w_root_unlock(root);
        break;
      } else {
        initiate_read = false;
      }
    }

    w_log(W_LOG_DBG, "waiting for change notifications");
    DWORD status = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    if (status == WAIT_OBJECT_0) {
      bytes = 0;
      if (!GetOverlappedResult(state->dir_handle, &olap,
            &bytes, FALSE)) {
        err = GetLastError();
        w_log(W_LOG_ERR, "overlapped ReadDirectoryChangesW(%s): 0x%x %s\n",
            root->root_path->buf,
            err, win32_strerror(err));

        if (err == ERROR_INVALID_PARAMETER && size > NETWORK_BUF_SIZE) {
          // May be a network buffer related size issue; the docs say that
          // we can hit this when watching a UNC path. Let's downsize and
          // retry the read just one time
          w_log(W_LOG_ERR, "retrying watch for possible network location %s "
              "with smaller buffer\n", root->root_path->buf);
          size = NETWORK_BUF_SIZE;
          initiate_read = true;
          continue;
        }

        if (err == ERROR_NOTIFY_ENUM_DIR) {
          w_root_schedule_recrawl(root, "ERROR_NOTIFY_ENUM_DIR");
        } else {
          w_log(W_LOG_ERR, "Cancelling watch for %s\n",
              root->root_path->buf);
          w_root_lock(root, "w_root_cancel");
          w_root_cancel(root);
          w_root_unlock(root);
          break;
        }
      } else {
        PFILE_NOTIFY_INFORMATION not = (PFILE_NOTIFY_INFORMATION)buf;
        struct winwatch_changed_item *head = NULL, *tail = NULL;
        while (true) {
          struct winwatch_changed_item *item;
          DWORD n_chars;
          w_string_t *name, *full;

          // FileNameLength is in BYTES, but FileName is WCHAR
          n_chars = not->FileNameLength / sizeof(not->FileName[0]);
          name = w_string_new_wchar_typed(not->FileName, n_chars, W_STRING_BYTE);

          full = w_string_path_cat(root->root_path, name);
          w_string_delref(name);

          if (w_is_ignored(root, full->buf, full->len)) {
            w_string_delref(full);
          } else {
            item = calloc(1, sizeof(*item));
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
          pthread_mutex_lock(&state->mtx);
          if (state->tail) {
            state->tail->next = head;
          } else {
            state->head = head;
          }
          state->tail = tail;
          pthread_mutex_unlock(&state->mtx);
          pthread_cond_signal(&state->cond);
        }
        ResetEvent(state->olap);
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

  pthread_mutex_lock(&state->mtx);
out:
  // Signal to winwatch_root_start that we're done initializing in
  // the failure path.  We'll also do this after we've completed
  // the run loop in the success path; it's a spurious wakeup but
  // harmless and saves us from adding and setting a control flag
  // in each of the failure `goto` statements. winwatch_root_dtor
  // will `pthread_join` us before `state` is freed.
  pthread_cond_signal(&state->cond);
  pthread_mutex_unlock(&state->mtx);

  if (buf) {
    free(buf);
  }
  w_log(W_LOG_DBG, "done\n");
  w_root_delref(root);
  return NULL;
}

static bool winwatch_root_start(w_root_t *root) {
  struct winwatch_root_state *state = root->watch;
  int err;
  unused_parameter(root);

  // Spin up the changes reading thread; it owns a ref on the root
  w_root_addref(root);

  // Acquire the mutex so thread initialization waits until we release it
  pthread_mutex_lock(&state->mtx);

  err = pthread_create(&state->thread, NULL, readchanges_thread, root);
  if (err == 0) {
    // Allow thread init to proceed; wait for its signal
    pthread_cond_wait(&state->cond, &state->mtx);
    pthread_mutex_unlock(&state->mtx);

    if (root->failure_reason) {
      w_log(W_LOG_ERR, "failed to start readchanges thread: %.*s\n",
          root->failure_reason->len, root->failure_reason->buf);
      return false;
    }
    return true;
  }

  pthread_mutex_unlock(&state->mtx);
  w_root_delref(root);
  w_log(W_LOG_ERR, "failed to start readchanges thread: %s\n", strerror(err));
  return false;
}

static bool winwatch_root_start_watch_file(w_root_t *root,
    struct watchman_file *file) {
  unused_parameter(file);
  unused_parameter(root);
  return true;
}

static void winwatch_root_stop_watch_file(w_root_t *root,
    struct watchman_file *file) {
  unused_parameter(file);
  unused_parameter(root);
}

static struct watchman_dir_handle *winwatch_root_start_watch_dir(
    w_root_t *root, struct watchman_dir *dir, struct timeval now,
    const char *path) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, strerror(errno));
    return NULL;
  }

  return osdir;
}

static void winwatch_root_stop_watch_dir(w_root_t *root,
    struct watchman_dir *dir) {
  unused_parameter(dir);
  unused_parameter(root);
}

static bool winwatch_root_consume_notify(w_root_t *root,
    struct watchman_pending_collection *coll)
{
  struct winwatch_root_state *state = root->watch;
  struct winwatch_changed_item *head, *item;
  struct timeval now;
  int n = 0;

  pthread_mutex_lock(&state->mtx);
  head = state->head;
  state->head = NULL;
  state->tail = NULL;
  pthread_mutex_unlock(&state->mtx);

  gettimeofday(&now, NULL);

  while (head) {
    item = head;
    head = head->next;
    n++;

    w_log(W_LOG_DBG, "readchanges: add pending %.*s\n",
        item->name->len, item->name->buf);
    w_pending_coll_add(coll, item->name, now, W_PENDING_VIA_NOTIFY);

    w_string_delref(item->name);
    free(item);
  }

  return n > 0;
}

static bool winwatch_root_wait_notify(w_root_t *root, int timeoutms) {
  struct winwatch_root_state *state = root->watch;
  struct timeval now, delta, target;
  struct timespec ts;

  if (timeoutms == 0 || state->head) {
    return state->head ? true : false;
  }

  // Add timeout to current time, convert to absolute timespec
  gettimeofday(&now, NULL);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, &ts);

  pthread_mutex_lock(&state->mtx);
  pthread_cond_timedwait(&state->cond, &state->mtx, &ts);
  pthread_mutex_unlock(&state->mtx);
  return state->head ? true : false;
}

static void winwatch_file_free(struct watchman_file *file) {
  unused_parameter(file);
}

struct watchman_ops win32_watcher = {
  "win32",
  WATCHER_HAS_PER_FILE_NOTIFICATIONS,
  winwatch_root_init,
  winwatch_root_start,
  winwatch_root_dtor,
  winwatch_root_start_watch_file,
  winwatch_root_stop_watch_file,
  winwatch_root_start_watch_dir,
  winwatch_root_stop_watch_dir,
  winwatch_root_signal_threads,
  winwatch_root_consume_notify,
  winwatch_root_wait_notify,
  winwatch_file_free
};

#endif // _WIN32

/* vim:ts=2:sw=2:et:
 */
