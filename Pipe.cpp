/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "Pipe.h"

#include <system_error>

namespace watchman {

Pipe::Pipe() {
#ifdef _WIN32
  HANDLE readPipe;
  HANDLE writePipe;
  SECURITY_ATTRIBUTES sec;

  memset(&sec, 0, sizeof(sec));
  sec.nLength = sizeof(sec);
  sec.bInheritHandle = FALSE; // O_CLOEXEC equivalent
  constexpr DWORD kPipeSize = 64 * 1024;

  if (!CreatePipe(&readPipe, &writePipe, &sec, kPipeSize)) {
    throw std::system_error(
        GetLastError(), std::system_category(), "CreatePipe failed");
  }
  read = FileDescriptor(intptr_t(readPipe));
  write = FileDescriptor(intptr_t(writePipe));

#else
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
#endif
}
}
