/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "FileDescriptor.h"
#include "Win32Handle.h"

namespace watchman {

// Convenience for constructing a Pipe
class Pipe {
 public:
#ifdef _WIN32
  Win32Handle read;
  Win32Handle write;
#else
  FileDescriptor read;
  FileDescriptor write;
#endif

  // Construct a pipe, setting the close-on-exec and
  // non-blocking bits.
  Pipe();
};
}
