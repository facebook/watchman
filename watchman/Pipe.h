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
