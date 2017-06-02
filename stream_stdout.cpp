/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

using watchman::FileDescriptor;

namespace {
class StdioStream : public watchman_stream {
  const FileDescriptor& fd_;

 public:
  explicit StdioStream(const FileDescriptor& fd) : fd_(fd) {}

  int read(void* buf, int size) override {
    auto result = fd_.read(buf, size);
    if (result.hasError()) {
      errno = result.error().value();
#ifdef _WIN32
      // TODO: propagate Result<int, std::error_code> as return type
      errno = map_win32_err(errno);
#endif
      return -1;
    }
    return result.value();
  }

  int write(const void* buf, int size) override {
    auto result = fd_.write(buf, size);
    if (result.hasError()) {
      errno = result.error().value();
#ifdef _WIN32
      // TODO: propagate Result<int, std::error_code> as return type
      errno = map_win32_err(errno);
#endif
      return -1;
    }
    return result.value();
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

  pid_t getPeerProcessID() const override {
    return 0;
  }

  const watchman::FileDescriptor& getFileDescriptor() const override {
    return fd_;
  }
};
}

w_stm_t w_stm_stdout(void) {
  static StdioStream stdoutStream(FileDescriptor::stdOut());
  return &stdoutStream;
}

w_stm_t w_stm_stdin(void) {
  static StdioStream stdinStream(FileDescriptor::stdIn());
  return &stdinStream;
}
