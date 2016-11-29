/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include "FileDescriptor.h"

using watchman::FileDescriptor;

struct watchman_event {
  int fd[2];
  bool is_pipe;
};

class UnixStream : public watchman_stream {
 public:
  FileDescriptor fd;
  struct watchman_event evt;

  explicit UnixStream(int descriptorNumber) : fd(descriptorNumber) {
    evt.fd[0] = fd.fd();
    evt.fd[1] = fd.fd();
    evt.is_pipe = false;
  }

  int read(void* buf, int size) override {
    errno = 0;
    return ::read(fd.fd(), buf, size);
  }

  int write(const void* buf, int size) override {
    errno = 0;
    return ::write(fd.fd(), buf, size);
  }

  w_evt_t getEvents() override {
    return &evt;
  }

  void setNonBlock(bool nonb) override {
    if (nonb) {
      fd.setNonBlock();
    } else {
      fd.clearNonBlock();
    }
  }

  bool rewind() override {
    return lseek(fd.fd(), 0, SEEK_SET) == 0;
  }

  bool shutdown() override {
    return ::shutdown(fd.fd(), SHUT_RDWR);
  }

  // For these PEERCRED things, the uid reported is the effective uid of
  // the process, which may have been altered due to setuid or similar
  // mechanisms.  We'll treat the other process as an owner if their
  // effective UID matches ours, or if they are root.
  bool peerIsOwner() override {
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd.fd(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
      if (cred.uid == getuid() || cred.uid == 0) {
        return true;
      }
    }
#elif defined(LOCAL_PEERCRED)
    struct xucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd.fd(), SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) == 0) {
      if (cred.cr_uid == getuid() || cred.cr_uid == 0) {
        return true;
      }
    }
#endif

    return false;
  }
};

w_evt_t w_event_make(void) {
  w_evt_t evt = (w_evt_t)calloc(1, sizeof(*evt));
  if (!evt) {
    return NULL;
  }
  if (pipe(evt->fd)) {
    free(evt);
    return NULL;
  }
  w_set_cloexec(evt->fd[0]);
  w_set_nonblock(evt->fd[0]);
  w_set_cloexec(evt->fd[1]);
  w_set_nonblock(evt->fd[1]);
  evt->is_pipe = true;
  return evt;
}

void w_event_set(w_evt_t evt) {
  if (!evt->is_pipe) {
    return;
  }
  ignore_result(write(evt->fd[1], "a", 1));
}

void w_event_destroy(w_evt_t evt) {
  if (!evt->is_pipe) {
    return;
  }
  close(evt->fd[0]);
  close(evt->fd[1]);
  free(evt);
}

bool w_event_test_and_clear(w_evt_t evt) {
  char buf[64];
  bool signalled = false;
  if (!evt->is_pipe) {
    return false;
  }
  while (read(evt->fd[0], buf, sizeof(buf)) > 0) {
    signalled = true;
  }
  return signalled;
}

int w_stm_fileno(w_stm_t stm) {
  auto unixStream = dynamic_cast<UnixStream*>(stm);
  if (!unixStream) {
    w_log(W_LOG_FATAL, "w_stm_fileno is only supported on a UnixStream\n");
  }
  return unixStream->fd.fd();
}

#define MAX_POLL_EVENTS 63 // Must match MAXIMUM_WAIT_OBJECTS-1 on win
int w_poll_events(struct watchman_event_poll *p, int n, int timeoutms) {
  struct pollfd pfds[MAX_POLL_EVENTS];
  int i;
  int res;

  if (n > MAX_POLL_EVENTS) {
    // Programmer error :-/
    w_log(W_LOG_FATAL, "%d > MAX_POLL_EVENTS (%d)\n", n, MAX_POLL_EVENTS);
  }

  for (i = 0; i < n; i++) {
    pfds[i].fd = p[i].evt->fd[0];
    pfds[i].events = POLLIN|POLLHUP|POLLERR;
    pfds[i].revents = 0;
  }

  res = poll(pfds, n, timeoutms);

  for (i = 0; i < n; i++) {
    p[i].ready = pfds[i].revents != 0;
  }

  return res;
}

w_stm_t w_stm_fdopen(int fd) {
  return new UnixStream(fd);
}

w_stm_t w_stm_connect_unix(const char *path, int timeoutms) {
  w_stm_t stm;
  struct sockaddr_un un;
  int max_attempts = timeoutms / 10;
  int attempts = 0;
  int bufsize = WATCHMAN_IO_BUF_SIZE;
  int fd;

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "w_stm_connect_unix(%s) path is too long\n", path);
    errno = E2BIG;
    return NULL;
  }

  fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (fd == -1) {
    return NULL;
  }

  memset(&un, 0, sizeof(un));
  un.sun_family = PF_LOCAL;
  memcpy(un.sun_path, path, strlen(path));

retry_connect:

  if (connect(fd, (struct sockaddr*)&un, sizeof(un))) {
    int err = errno;

    if (err == ECONNREFUSED || err == ENOENT) {
      if (attempts++ < max_attempts) {
        usleep(10000);
        goto retry_connect;
      }
    }

    close(fd);
    return NULL;
  }

  setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
      (void*)&bufsize, sizeof(bufsize));

  stm = w_stm_fdopen(fd);
  if (!stm) {
    close(fd);
  }
  return stm;
}

w_stm_t w_stm_open(const char *filename, int flags, ...) {
  int mode = 0;
  int fd;
  w_stm_t stm;

  // If we're creating, pull out the mode flag
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }

  fd = open(filename, flags, mode);
  if (fd == -1) {
    return NULL;
  }

  stm = w_stm_fdopen(fd);
  if (!stm) {
    close(fd);
  }
  return stm;
}
