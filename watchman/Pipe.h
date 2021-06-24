/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman/FileDescriptor.h"

namespace watchman {

// Convenience for constructing a Pipe
class Pipe {
 public:
  FileDescriptor read;
  FileDescriptor write;

  // Construct a pipe, setting the close-on-exec and
  // non-blocking bits.
  Pipe();
};

// Convenience for constructing a SocketPair
class SocketPair {
 public:
  FileDescriptor read;
  FileDescriptor write;

  // Construct a socketpair, setting the close-on-exec and
  // non-blocking bits.
  SocketPair();
};

} // namespace watchman
