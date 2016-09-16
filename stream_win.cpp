/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Things are more complicated here than on unix.
// We maintain an overlapped context for reads and
// another for writes.  Actual write data is queued
// and dispatched to the underlying handle as prior
// writes complete.

struct win_handle;

struct overlapped_op {
  OVERLAPPED olap;
  struct win_handle *h;
  struct write_buf *wbuf;
};

struct write_buf {
  struct write_buf *next;
  int len;
  char *cursor;
  char data[1];
};

struct win_handle {
  struct overlapped_op *read_pending, *write_pending;
  HANDLE h, waitable;
  CRITICAL_SECTION mtx;
  bool error_pending;
  DWORD errcode;
  DWORD file_type;
  struct write_buf *write_head, *write_tail;
  char read_buf[8192];
  char *read_cursor;
  int read_avail;
  bool blocking;
};

#if 1
#define stream_debug(x, ...) 0
#else
#define stream_debug(x, ...)                                                   \
  do {                                                                         \
    time_t now;                                                                \
    char timebuf[64];                                                          \
    struct tm tm;                                                              \
    time(&now);                                                                \
    localtime_s(&tm, &now);                                                    \
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);              \
    fprintf(stderr, "%s    : ", timebuf);                                      \
    fprintf(stderr, x, __VA_ARGS__);                                           \
    fflush(stderr);                                                            \
  } while (0)
#endif

typedef BOOL (WINAPI *get_overlapped_result_ex_func)(
    HANDLE file,
    LPOVERLAPPED olap,
    LPDWORD bytes,
    DWORD millis,
    BOOL alertable);
static BOOL WINAPI probe_get_overlapped_result_ex(
    HANDLE file,
    LPOVERLAPPED olap,
    LPDWORD bytes,
    DWORD millis,
    BOOL alertable);
static get_overlapped_result_ex_func get_overlapped_result_ex =
    probe_get_overlapped_result_ex;

static BOOL WINAPI get_overlapped_result_ex_impl(
    HANDLE file,
    LPOVERLAPPED olap,
    LPDWORD bytes,
    DWORD millis,
    BOOL alertable) {

  DWORD waitReturnCode, err;

  stream_debug( "Preparing to wait for maximum %ums\n", millis );
  if ( millis != 0 ) {

    waitReturnCode = WaitForSingleObjectEx(olap->hEvent, millis, alertable);
    switch (waitReturnCode)
    {
    case WAIT_OBJECT_0:
      // Event is signaled, overlapped IO operation result should be available.
      break;
    case WAIT_IO_COMPLETION:
      // WaitForSingleObjectEx returnes because the system added an I/O
      // completion routine or an asynchronous procedure call (APC) to the
      // thread queue.
      SetLastError(WAIT_IO_COMPLETION);
      break;
    case WAIT_TIMEOUT:
      // We reached the maximum allowed wait time, the IO operation failed
      // to complete in timely fashion.
      SetLastError(WAIT_TIMEOUT);
      return FALSE;

    case WAIT_FAILED:
      // something went wrong calling WaitForSingleObjectEx
      err = GetLastError();
      stream_debug("WaitForSingleObjectEx failed: %s\n", win32_strerror(err));
      return FALSE;

    default:
      // unexpected situation deserving investigation.
      err = GetLastError();
      stream_debug("Unexpected error: %s\n", win32_strerror(err));
      return FALSE;
    }
  }

  return GetOverlappedResult(file, olap, bytes, FALSE);
}


static BOOL WINAPI probe_get_overlapped_result_ex(
    HANDLE file,
    LPOVERLAPPED olap,
    LPDWORD bytes,
    DWORD millis,
    BOOL alertable ) {
  get_overlapped_result_ex_func func;

  func = (get_overlapped_result_ex_func)GetProcAddress(
      GetModuleHandle("kernel32.dll"),
      "GetOverlappedResultEx");

  if ((getenv("WATCHMAN_WIN7_COMPAT") &&
       getenv("WATCHMAN_WIN7_COMPAT")[0] == '1') || !func) {
    func = get_overlapped_result_ex_impl;
  }

  get_overlapped_result_ex = func;

  return func(file, olap, bytes, millis, alertable);
}


static int win_close(w_stm_t stm) {
  auto h = (win_handle*)stm->handle;

  EnterCriticalSection(&h->mtx);

  if (h->read_pending) {
    if (CancelIoEx(h->h, &h->read_pending->olap)) {
      free(h->read_pending);
      h->read_pending = NULL;
    }
  }
  if (h->write_pending) {
    if (CancelIoEx(h->h, &h->write_pending->olap)) {
      free(h->write_pending);
      h->write_pending = NULL;
    }

    while (h->write_head) {
      struct write_buf *b = h->write_head;
      h->write_head = b->next;

      free(b->data);
      free(b);
    }
  }

  if (h->h != INVALID_HANDLE_VALUE) {
    CloseHandle(h->h);
    h->h = INVALID_HANDLE_VALUE;
  }

  if (h->waitable) {
    CloseHandle(h->waitable);
  }
  free(h);
  stm->handle = NULL;

  return 0;
}

static void move_from_read_buffer(struct win_handle *h,
    int *total_read_ptr,
    char **target_buf_ptr,
    int *size_ptr) {
  int nread = MIN(*size_ptr, h->read_avail);
  size_t wasted;

  if (!nread) {
    return;
  }

  memcpy(*target_buf_ptr, h->read_cursor, nread);
  *total_read_ptr += nread;
  *target_buf_ptr += nread;
  *size_ptr -= nread;
  h->read_cursor += nread;
  h->read_avail -= nread;

  stream_debug("moved %d bytes from buffer\n", nread);

  // Pack the buffer to free up space at the rear for reads
  wasted = h->read_cursor - h->read_buf;
  if (wasted) {
    memmove(h->read_buf, h->read_cursor, h->read_avail);
    h->read_cursor = h->read_buf;
  }
}

static bool win_read_handle_completion(struct win_handle *h) {
  BOOL olap_res;
  DWORD bytes, err;

again:

  EnterCriticalSection(&h->mtx);
  if (!h->read_pending) {
    LeaveCriticalSection(&h->mtx);
    return false;
  }

  stream_debug("have read_pending, checking status\n");
  ResetEvent(h->waitable);

  // Don't hold the mutex while we're blocked
  LeaveCriticalSection(&h->mtx);
  olap_res = get_overlapped_result_ex(h->h, &h->read_pending->olap, &bytes,
      h->blocking ? INFINITE : 0, true);
  err = GetLastError();
  EnterCriticalSection(&h->mtx);

  if (olap_res) {
    stream_debug("pending read completed, read %d bytes, %s\n",
        (int)bytes, win32_strerror(err));
    h->read_avail += bytes;
    free(h->read_pending);
    h->read_pending = NULL;
  } else {
    if (err == WAIT_IO_COMPLETION) {
      // Some other async thing completed and our wait was interrupted.
      // This is similar to EINTR
      LeaveCriticalSection(&h->mtx);
      goto again;
    }
    stream_debug("pending read failed: %s\n", win32_strerror(err));
    if (err != ERROR_IO_INCOMPLETE) {
      // Failed
      free(h->read_pending);
      h->read_pending = NULL;

      h->errcode = err;
      h->error_pending = true;
      stream_debug("marking read as failed\n");
      SetEvent(h->waitable);
    }
  }
  LeaveCriticalSection(&h->mtx);

  return h->read_pending != NULL;
}

static int win_read_blocking(struct win_handle* h, void* buf, int size) {
  int total_read = 0;
  DWORD bytes, err;

  move_from_read_buffer(h, &total_read, (char**)&buf, &size);

  if (size == 0) {
    return total_read;
  }

  stream_debug("blocking read of %d bytes\n", (int)size);
  if (ReadFile(h->h, buf, size, &bytes, NULL)) {
    total_read += bytes;
    stream_debug("blocking read provided %d bytes, total=%d\n",
        (int)bytes, total_read);
    return total_read;
  }

  err = GetLastError();

  stream_debug("blocking read failed: %s\n", win32_strerror(err));

  if (total_read) {
    stream_debug("but already got %d bytes from buffer\n", total_read);
    return total_read;
  }

  errno = map_win32_err(err);
  return -1;
}

static int win_read_non_blocking(struct win_handle* h, void* buf, int size) {
  int total_read = 0;
  char *target;
  DWORD target_space;
  DWORD bytes;

  stream_debug("non_blocking read for %d bytes\n", size);

  move_from_read_buffer(h, &total_read, (char**)&buf, &size);

  target = h->read_cursor + h->read_avail;
  target_space = (DWORD)((h->read_buf + sizeof(h->read_buf)) - target);

  stream_debug("initiate read for %d\n", target_space);

  // Create a unique olap for each request
  h->read_pending = (overlapped_op*)calloc(1, sizeof(*h->read_pending));
  if (h->read_avail == 0) {
    stream_debug("ResetEvent because there is no read_avail right now\n");
    ResetEvent(h->waitable);
  }
  h->read_pending->olap.hEvent = h->waitable;
  h->read_pending->h = h;

  if (!ReadFile(h->h, target, target_space, NULL, &h->read_pending->olap)) {
    DWORD err = GetLastError();

    if (err != ERROR_IO_PENDING) {
      free(h->read_pending);
      h->read_pending = NULL;

      stream_debug("olap read failed immediately: %s\n",
          win32_strerror(err));
      SetEvent(h->waitable);
    } else {
      stream_debug("olap read queued ok\n");
    }

    errno = map_win32_err(err);
    return total_read == 0 ? -1 : total_read;
  }

  // Note: we obtain the bytes via GetOverlappedResult because the docs for
  // ReadFile warn against passing the pointer to the ReadFile parameter for
  // asynchronouse reads
  GetOverlappedResult(h->h, &h->read_pending->olap, &bytes, FALSE);
  stream_debug("olap read succeeded immediately bytes=%d\n", (int)bytes);

  h->read_avail += bytes;
  free(h->read_pending);
  h->read_pending = NULL;

  move_from_read_buffer(h, &total_read, (char**)&buf, &size);

  stream_debug("read returning %d\n", total_read);
  SetEvent(h->waitable);
  return total_read;
}

static int win_read(w_stm_t stm, void *buf, int size) {
  auto h = (win_handle*)stm->handle;

  if (win_read_handle_completion(h)) {
    errno = EAGAIN;
    return -1;
  }

  // Report a prior failure
  if (h->error_pending) {
    stream_debug("win_read: reporting prior failure err=%d errno=%d %s\n",
                 h->errcode, map_win32_err(h->errcode),
                 win32_strerror(h->errcode));
    errno = map_win32_err(h->errcode);
    h->error_pending = false;
    return -1;
  }

  if (h->blocking) {
    return win_read_blocking(h, buf, size);
  }

  return win_read_non_blocking(h, buf, size);
}

static void initiate_write(struct win_handle *h);

static void CALLBACK write_completed(DWORD err, DWORD bytes,
    LPOVERLAPPED olap) {
  // Reverse engineer our handle from the olap pointer
  struct overlapped_op *op = (overlapped_op*)olap;
  struct win_handle *h = op->h;
  struct write_buf *wbuf = op->wbuf;

  stream_debug("WriteFileEx: completion callback invoked: bytes=%d %s\n",
      (int)bytes, win32_strerror(err));

  EnterCriticalSection(&h->mtx);
  if (h->write_pending == op) {
    h->write_pending = NULL;
  }

  if (err == 0) {
    wbuf->cursor += bytes;
    wbuf->len -= bytes;

    if (wbuf->len == 0) {
      // Consumed this buffer
      free(wbuf);
    } else {
      stream_debug("WriteFileEx: short write: %d written, %d remain\n",
              bytes, wbuf->len);
      // the initiate_write call will send the remainder
      // but we need to re-insert this wbuf in the write queue
      wbuf->next = h->write_head;
      h->write_head = wbuf;
      if (!h->write_tail) {
        h->write_tail = wbuf;
      }
    }
  } else {
    stream_debug("WriteFilex: completion: failed: %s\n",
        win32_strerror(err));
    h->errcode = err;
    h->error_pending = true;
  }

  stream_debug("SetEvent because WriteFileEx completed\n");
  SetEvent(h->waitable);

  // Send whatever else we have waiting to go
  initiate_write(h);

  LeaveCriticalSection(&h->mtx);

  // Free the prior struct after possibly initiating another write
  // to minimize the chance of the same address being reused and
  // confusing the completion status
  free(op);
}

// Must be called with the mutex held
static void initiate_write(struct win_handle *h) {
  struct write_buf *wbuf = h->write_head;
  if (h->write_pending || !wbuf) {
    return;
  }

  h->write_head = wbuf->next;
  if (!h->write_head) {
    h->write_tail = NULL;
  }

  h->write_pending = (overlapped_op*)calloc(1, sizeof(*h->write_pending));
  h->write_pending->h = h;
  h->write_pending->wbuf = wbuf;

  stream_debug(
      "Calling WriteFileEx with wbuf=%p wbuf->cursor=%p len=%d olap=%p\n", wbuf,
      wbuf->cursor, wbuf->len, &h->write_pending->olap);
  if (!WriteFileEx(h->h, wbuf->cursor, wbuf->len, &h->write_pending->olap,
        write_completed)) {
    stream_debug("WriteFileEx: failed %s\n",
        win32_strerror(GetLastError()));
    free(h->write_pending);
    h->write_pending = NULL;
  } else {
    stream_debug("WriteFileEx: queued %d bytes for later\n", wbuf->len);
  }
}

static int win_write(w_stm_t stm, const void *buf, int size) {
  auto h = (win_handle*)stm->handle;
  struct write_buf *wbuf;

  EnterCriticalSection(&h->mtx);
  if (h->file_type != FILE_TYPE_PIPE && h->blocking && !h->write_head) {
    DWORD bytes;
    stream_debug("blocking write of %d\n", size);
    if (WriteFile(h->h, buf, size, &bytes, NULL)) {
      LeaveCriticalSection(&h->mtx);
      stream_debug("blocking write wrote %d bytes of %d\n", bytes, size);
      return bytes;
    }
    h->errcode = GetLastError();
    h->error_pending = true;
    errno = map_win32_err(h->errcode);
    stream_debug("SetEvent because blocking write completed (failed)\n");
    SetEvent(h->waitable);
    stream_debug("write failed: %s\n", win32_strerror(h->errcode));
    LeaveCriticalSection(&h->mtx);
    return -1;
  }

  wbuf = (write_buf*)malloc(sizeof(*wbuf) + size - 1);
  if (!wbuf) {
    return -1;
  }
  wbuf->next = NULL;
  wbuf->cursor = wbuf->data;
  wbuf->len = size;
  memcpy(wbuf->data, buf, size);

  if (h->write_tail) {
    h->write_tail->next = wbuf;
  } else {
    h->write_head = wbuf;
  }
  h->write_tail = wbuf;

  stream_debug("queue write of %d bytes to write_tail\n", size);

  if (!h->write_pending) {
    initiate_write(h);
  }

  LeaveCriticalSection(&h->mtx);

  return size;
}

static void win_get_events(w_stm_t stm, w_evt_t *readable) {
  auto h = (win_handle*)stm->handle;
  *readable = (w_evt_t)h->waitable;
}

static void win_set_nonb(w_stm_t stm, bool nonb) {
  auto h = (win_handle*)stm->handle;
  h->blocking = !nonb;
}

static bool win_rewind(w_stm_t stm) {
  auto h = (win_handle*)stm->handle;
  bool res;
  LARGE_INTEGER new_pos;

  new_pos.QuadPart = 0;
  res = SetFilePointerEx(h->h, new_pos, &new_pos, FILE_BEGIN);
  errno = map_win32_err(GetLastError());
  return res;
}

// Ensure that any data buffered for write are sent prior to setting
// ourselves up to close
static bool win_shutdown(w_stm_t stm) {
  auto h = (win_handle*)stm->handle;
  BOOL olap_res;
  DWORD bytes;

  h->blocking = true;
  while (h->write_pending) {
    olap_res = get_overlapped_result_ex(h->h, &h->write_pending->olap,
        &bytes, INFINITE, true);
  }

  return true;
}

static bool win_peer_is_owner(w_stm_t stm) {
  unused_parameter(stm);
  // TODO: implement this for Windows
  return true;
}

static struct watchman_stream_ops win_ops = {
  win_close,
  win_read,
  win_write,
  win_get_events,
  win_set_nonb,
  win_rewind,
  win_shutdown,
  win_peer_is_owner
};

w_evt_t w_event_make(void) {
  return (w_evt_t)CreateEvent(NULL, TRUE, FALSE, NULL);
}

void w_event_set(w_evt_t evt) {
  SetEvent(evt);
}

void w_event_destroy(w_evt_t evt) {
  CloseHandle(evt);
}

bool w_event_test_and_clear(w_evt_t evt) {
  bool was_set = WaitForSingleObject(evt, 0) == WAIT_OBJECT_0;
  ResetEvent(evt);
  return was_set;
}

w_stm_t w_stm_handleopen(HANDLE handle) {
  w_stm_t stm;
  struct win_handle *h;

  if (handle == INVALID_HANDLE_VALUE || handle == NULL) {
    return NULL;
  }

  stm = (w_stm_t)calloc(1, sizeof(*stm));
  if (!stm) {
    return NULL;
  }

  h = (win_handle*)calloc(1, sizeof(*h));
  if (!h) {
    free(stm);
    return NULL;
  }

  InitializeCriticalSection(&h->mtx);
  h->read_cursor = h->read_buf;
  h->blocking = true;
  h->h = handle;
  // Initially signalled, meaning that they can try reading
  h->waitable = CreateEvent(NULL, TRUE, TRUE, NULL);
  stm->handle = h;
  stm->ops = &win_ops;
  h->file_type = GetFileType(handle);

  return stm;
}

w_stm_t w_stm_connect_named_pipe(const char *path, int timeoutms) {
  w_stm_t stm = NULL;
  HANDLE handle;
  DWORD err;
  DWORD64 deadline = GetTickCount64() + timeoutms;

  if (strlen(path) > 255) {
    w_log(W_LOG_ERR, "w_stm_connect_named_pipe(%s) path is too long\n", path);
    errno = E2BIG;
    return NULL;
  }

retry_connect:
  handle = CreateFile(path,
      GENERIC_READ|GENERIC_WRITE,
      0,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED,
      NULL);

  if (handle != INVALID_HANDLE_VALUE) {
    stm = w_stm_handleopen(handle);
    if (!stm) {
      CloseHandle(handle);
    }
    return stm;
  }

  err = GetLastError();
  if (timeoutms > 0) {
    timeoutms -= (DWORD)(GetTickCount64() - deadline);
  }
  if (timeoutms <= 0 || (err != ERROR_PIPE_BUSY &&
        err != ERROR_FILE_NOT_FOUND)) {
    // either we're out of time, or retrying won't help with this error
    errno = map_win32_err(err);
    return NULL;
  }

  // We can retry
  if (!WaitNamedPipe(path, timeoutms)) {
    err = GetLastError();
    if (err == ERROR_SEM_TIMEOUT) {
      errno = map_win32_err(err);
      return NULL;
    }
    if (err == ERROR_FILE_NOT_FOUND) {
      // Grace to allow it to be created
      SleepEx(10, true);
    }
  }

  goto retry_connect;
}

int w_poll_events(struct watchman_event_poll *p, int n, int timeoutms) {
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  int i;
  DWORD res;

  if (n > MAXIMUM_WAIT_OBJECTS - 1) {
    // Programmer error :-/
    w_log(W_LOG_FATAL, "%d > MAXIMUM_WAIT_OBJECTS-1 (%d)\n", n,
        MAXIMUM_WAIT_OBJECTS - 1);
  }

  for (i = 0; i < n; i++) {
    handles[i] = p[i].evt;
    p[i].ready = false;
  }

  res = WaitForMultipleObjectsEx(n, handles, false,
          timeoutms == -1 ? INFINITE : timeoutms, true);

  if (res == WAIT_FAILED) {
    errno = map_win32_err(GetLastError());
    return -1;
  }
  if (res == WAIT_IO_COMPLETION) {
    errno = EINTR;
    return -1;
  }
  // Note: WAIT_OBJECT_0 == 0
  if (/* res >= WAIT_OBJECT_0 && */ res < WAIT_OBJECT_0 + n) {
    p[res - WAIT_OBJECT_0].ready = true;
    return 1;
  }
  if (res >= WAIT_ABANDONED_0 && res < WAIT_ABANDONED_0 + n) {
    p[res - WAIT_ABANDONED_0].ready = true;
    return 1;
  }
  return 0;
}

// similar to open(2), but returns a handle
HANDLE w_handle_open(const char *path, int flags) {
  DWORD access = 0, share = 0, create = 0, attrs = 0;
  DWORD err;
  SECURITY_ATTRIBUTES sec;
  WCHAR *wpath;
  HANDLE h;

  if (!strcmp(path, "/dev/null")) {
    path = "NUL:";
  }

  wpath = w_utf8_to_win_unc(path, -1);
  if (!wpath) {
    return INVALID_HANDLE_VALUE;
  }

  if (flags & (O_WRONLY|O_RDWR)) {
    access |= GENERIC_WRITE;
  }
  if ((flags & O_WRONLY) == 0) {
    access |= GENERIC_READ;
  }

  // We want more posix-y behavior by default
  share = FILE_SHARE_DELETE|FILE_SHARE_READ|FILE_SHARE_WRITE;

  memset(&sec, 0, sizeof(sec));
  sec.nLength = sizeof(sec);
  sec.bInheritHandle = TRUE;
  if (flags & O_CLOEXEC) {
    sec.bInheritHandle = FALSE;
  }

  if ((flags & (O_CREAT|O_EXCL)) == (O_CREAT|O_EXCL)) {
    create = CREATE_NEW;
  } else if ((flags & (O_CREAT|O_TRUNC)) == (O_CREAT|O_TRUNC)) {
    create = CREATE_ALWAYS;
  } else if (flags & O_CREAT) {
    create = OPEN_ALWAYS;
  } else if (flags & O_TRUNC) {
    create = TRUNCATE_EXISTING;
  } else {
    create = OPEN_EXISTING;
  }

  attrs = FILE_ATTRIBUTE_NORMAL;
  if (flags & O_DIRECTORY) {
    attrs |= FILE_FLAG_BACKUP_SEMANTICS;
  }

  h = CreateFileW(wpath, access, share, &sec, create, attrs, NULL);
  err = GetLastError();
  free(wpath);

  errno = map_win32_err(err);
  return h;
}

w_stm_t w_stm_open(const char *path, int flags, ...) {
  w_stm_t stm;
  HANDLE h = w_handle_open(path, flags);

  if (h == INVALID_HANDLE_VALUE) {
    return NULL;
  }

  stm = w_stm_handleopen(h);
  if (!stm) {
    CloseHandle(h);
  }
  return stm;
}

HANDLE w_stm_handle(w_stm_t stm) {
  auto h = (win_handle*)stm->handle;
  return h->h;
}
