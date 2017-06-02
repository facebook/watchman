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
#include "Pipe.h"
#include "make_unique.h"

using watchman::FileDescriptor;
using watchman::Pipe;

static const int kWriteTimeout = 60000;

namespace {
// This trait allows w_poll_events to wait on either a PipeEvent or
// a descriptor contained in a UnixStream
class PollableEvent : public watchman_event {
 public:
  virtual int getFd() const = 0;
};

// The event object, implemented as pipe
class PipeEvent : public PollableEvent {
 public:
  Pipe pipe;

  void notify() override {
    ignore_result(write(pipe.write.fd(), "a", 1));
  }

  bool testAndClear() override {
    char buf[64];
    bool signalled = false;
    while (read(pipe.read.fd(), buf, sizeof(buf)) > 0) {
      signalled = true;
    }
    return signalled;
  }

  int getFd() const override {
    return pipe.read.fd();
  }
};

// Event object that UnixStream returns via getEvents.
// It cannot be poked by hand; it is just a helper to
// allow waiting on a socket using w_poll_events.
class FakeSocketEvent : public PollableEvent {
 public:
  int socket;

  explicit FakeSocketEvent(int fd) : socket(fd) {}

  void notify() override {}
  bool testAndClear() override {
    return false;
  }
  int getFd() const override {
    return socket;
  }
};

class UnixStream : public watchman_stream {
 public:
  FileDescriptor fd;
  FakeSocketEvent evt;
#ifdef SO_PEERCRED
  struct ucred cred;
#elif defined(LOCAL_PEERCRED)
  struct xucred cred;
#endif
  bool credvalid{false};

  explicit UnixStream(FileDescriptor&& descriptor)
      : fd(std::move(descriptor)), evt(fd.fd()) {
    socklen_t len = sizeof(cred);
#ifdef SO_PEERCRED
    credvalid = getsockopt(fd.fd(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0;
#elif defined(LOCAL_PEERCRED)
    credvalid =
        getsockopt(fd.fd(), SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) == 0;
#endif
  }

  const FileDescriptor& getFileDescriptor() const override {
    return fd;
  }

  int read(void* buf, int size) override {
    errno = 0;
    return ::read(fd.fd(), buf, size);
  }

  int write(const void* buf, int size) override {
    errno = 0;
    if (!fd.isNonBlock()) {
      int wrote = 0;

      while (size > 0) {
        struct pollfd pfd;
        pfd.fd = fd.fd();
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, kWriteTimeout) == 0) {
          break;
        }
        if (pfd.revents & (POLLERR | POLLHUP)) {
          break;
        }
        auto x = ::write(fd.fd(), buf, size);
        if (x <= 0) {
          break;
        }

        wrote += x;
        size -= x;
        buf = reinterpret_cast<const void*>(
            reinterpret_cast<const char*>(buf) + x);
      }
      return wrote == 0 ? -1 : wrote;
    }
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
    if (!credvalid) {
      return false;
    }
#ifdef SO_PEERCRED
    if (cred.uid == getuid() || cred.uid == 0) {
      return true;
    }
#elif defined(LOCAL_PEERCRED)
    if (cred.cr_uid == getuid() || cred.cr_uid == 0) {
      return true;
    }
#endif
    return false;
  }

  pid_t getPeerProcessID() const override {
    if (!credvalid) {
      return 0;
    }
#ifdef SO_PEERCRED
    return cred.pid;
#else
    return 0;
#endif
  }
};
}

std::unique_ptr<watchman_event> w_event_make(void) {
  return watchman::make_unique<PipeEvent>();
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
    auto pe = dynamic_cast<PollableEvent*>(p[i].evt);
    w_check(pe != nullptr, "PollableEvent!?");
    pfds[i].fd = pe->getFd();
    pfds[i].events = POLLIN|POLLHUP|POLLERR;
    pfds[i].revents = 0;
  }

  res = poll(pfds, n, timeoutms);

  for (i = 0; i < n; i++) {
    p[i].ready = pfds[i].revents != 0;
  }

  return res;
}

std::unique_ptr<watchman_stream> w_stm_fdopen(FileDescriptor&& fd) {
  if (!fd) {
    return nullptr;
  }
  return watchman::make_unique<UnixStream>(std::move(fd));
}

std::unique_ptr<watchman_stream> w_stm_connect_unix(
    const char* path,
    int timeoutms) {
  struct sockaddr_un un;
  int max_attempts = timeoutms / 10;
  int attempts = 0;
  int bufsize = WATCHMAN_IO_BUF_SIZE;

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "w_stm_connect_unix(%s) path is too long\n", path);
    errno = E2BIG;
    return NULL;
  }

  FileDescriptor fd(socket(PF_LOCAL, SOCK_STREAM, 0));
  if (!fd) {
    return nullptr;
  }

  memset(&un, 0, sizeof(un));
  un.sun_family = PF_LOCAL;
  memcpy(un.sun_path, path, strlen(path));

retry_connect:

  if (connect(fd.fd(), (struct sockaddr*)&un, sizeof(un))) {
    int err = errno;

    if (err == ECONNREFUSED || err == ENOENT) {
      if (attempts++ < max_attempts) {
        usleep(10000);
        goto retry_connect;
      }
    }

    return nullptr;
  }

  setsockopt(fd.fd(), SOL_SOCKET, SO_RCVBUF, (void*)&bufsize, sizeof(bufsize));

  return w_stm_fdopen(std::move(fd));
}

std::unique_ptr<watchman_stream>
w_stm_open(const char* filename, int flags, ...) {
  int mode = 0;

  // If we're creating, pull out the mode flag
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }

  return w_stm_fdopen(FileDescriptor(open(filename, flags, mode)));
}
