/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#ifndef WATCHMAN_STREAM_H
#define WATCHMAN_STREAM_H

#include "FileDescriptor.h"

// Very limited stream abstraction to make it easier to
// deal with portability between Windows and POSIX.

class watchman_event {
 public:
  virtual ~watchman_event() = default;
  virtual void notify() = 0;
  virtual bool testAndClear() = 0;
};

using w_evt_t = watchman_event*;

class watchman_stream {
 public:
  virtual ~watchman_stream() = default;
  virtual int read(void* buf, int size) = 0;
  virtual int write(const void* buf, int size) = 0;
  virtual w_evt_t getEvents() = 0;
  virtual void setNonBlock(bool nonBlock) = 0;
  virtual bool rewind() = 0;
  virtual bool shutdown() = 0;
  virtual bool peerIsOwner() = 0;
  virtual pid_t getPeerProcessID() const = 0;
  virtual const watchman::FileDescriptor& getFileDescriptor() const = 0;
};
using w_stm_t = watchman_stream*;

struct watchman_event_poll {
  watchman_event* evt;
  bool ready;
};

// Make a event that can be manually signalled
std::unique_ptr<watchman_event> w_event_make(void);

// Go to sleep for up to timeoutms.
// Returns sooner if any of the watchman_event objects referenced
// in the array P are signalled
int w_poll_events(struct watchman_event_poll *p, int n, int timeoutms);

// Create a connected unix socket or a named pipe client stream
std::unique_ptr<watchman_stream> w_stm_connect(const char* path, int timeoutms);

w_stm_t w_stm_stdout(void);
w_stm_t w_stm_stdin(void);
#ifndef _WIN32
std::unique_ptr<watchman_stream> w_stm_connect_unix(
    const char* path,
    int timeoutms);
#else
std::unique_ptr<watchman_stream> w_stm_connect_named_pipe(
    const char* path,
    int timeoutms);
watchman::FileDescriptor w_handle_open(const char* path, int flags);
#endif
std::unique_ptr<watchman_stream> w_stm_fdopen(watchman::FileDescriptor&& fd);
std::unique_ptr<watchman_stream> w_stm_open(const char* path, int flags, ...);

// Make a temporary file name and open it.
// Marks the file as CLOEXEC
std::unique_ptr<watchman_stream> w_mkstemp(char* templ);

#endif
