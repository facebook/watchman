/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

namespace {
class StdioStream : public watchman_stream {
  int fd;

 public:
  explicit StdioStream(int fd) : fd(fd) {}

  int read(void* buf, int size) override {
    return ::read(fd, buf, size);
  }

  int write(const void* buf, int size) override {
    return ::write(fd, buf, size);
  }

  w_evt_t getEvents() override {
    w_log(W_LOG_FATAL, "calling get_events on a stdio stm\n");
    return nullptr;
  }

  void setNonBlock(bool) override {}

  bool rewind() override {
    return false;
  }

  bool shutdown() override {
    return false;
  }

  bool peerIsOwner() override {
    return false;
  }

#ifndef _WIN32
  int getFileDescriptor() const override {
    return fd;
  }
#else
  intptr_t getWindowsHandle() const override {
    return _get_osfhandle(fd);
  }
#endif
};

StdioStream stdoutStream(STDOUT_FILENO);
StdioStream stdinStream(STDIN_FILENO);
}

w_stm_t w_stm_stdout(void) {
  return &stdoutStream;
}

w_stm_t w_stm_stdin(void) {
  return &stdinStream;
}
