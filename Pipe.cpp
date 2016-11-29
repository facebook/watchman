/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "Pipe.h"

#include <system_error>

namespace watchman {

#ifdef _WIN32
static inline int pipe(int fd[2]) {
  return _pipe(fd, 64 * 1024, O_BINARY);
}
#endif

Pipe::Pipe() {
  int fds[2];
  int res;
#if HAVE_PIPE2
  res = pipe2(fds, O_NONBLOCK | O_CLOEXEC);
#else
  res = pipe(fds);
#endif

  if (res) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string("pipe error: ") + strerror(errno));
  }
  read = FileDescriptor(fds[0]);
  write = FileDescriptor(fds[1]);

#if !HAVE_PIPE2
  read.setCloExec();
  read.setNonBlock();
  write.setCloExec();
  write.setNonBlock();
#endif
}
}
