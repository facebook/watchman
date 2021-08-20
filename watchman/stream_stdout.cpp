/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/Logging.h"
#include "watchman/watchman_stream.h"

using watchman::FileDescriptor;
using namespace watchman;

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
    log(FATAL, "calling get_events on a stdio stm\n");
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
} // namespace

w_stm_t w_stm_stdout() {
  static StdioStream stdoutStream(FileDescriptor::stdOut());
  return &stdoutStream;
}

w_stm_t w_stm_stdin() {
  static StdioStream stdinStream(FileDescriptor::stdIn());
  return &stdinStream;
}
