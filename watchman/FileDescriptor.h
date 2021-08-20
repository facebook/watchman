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
#include <system_error>
#include "watchman/Result.h"
#include "watchman/watchman_system.h"

class w_string;

namespace watchman {

struct FileInformation;

// Manages the lifetime of a system independent file descriptor.
// On POSIX systems this is a posix file descriptor.
// On Win32 systems this is a Win32 HANDLE object.
// It will close() the descriptor when it is destroyed.
class FileDescriptor {
 public:
  using system_handle_type =
#ifdef _WIN32
      // We track the HANDLE value as intptr_t to avoid needing
      // to pull in the windows header files all over the place;
      // this is consistent with the _get_osfhandle function in
      // the msvcrt library.
      intptr_t
#else
      int
#endif
      ;
  enum class FDType {
    Unknown,
    Generic,
    Pipe,
    Socket,
  };

  // A value representing the canonical invalid handle
  // value for the system.
  static constexpr system_handle_type kInvalid = -1;

  // Normalizes invalid handle values to our canonical invalid handle value.
  // Otherwise, just returns the handle as-is.
  static system_handle_type normalizeHandleValue(system_handle_type h);

  // If the FDType is Unknown, probe it to determine its type
  static FDType resolveFDType(system_handle_type h, FDType fdType);

  ~FileDescriptor();

  // Default construct to an empty instance
  FileDescriptor() = default;

  // Construct a file descriptor object from an fd.
  // Will happily accept an invalid handle value without
  // raising an error; the FileDescriptor will simply evaluate as
  // false in a boolean context.
  explicit FileDescriptor(system_handle_type fd, FDType fdType);

  // Construct a file descriptor object from an fd.
  // If fd is invalid will throw a generic error with a message
  // constructed from the provided operation name and the current
  // errno value.
  FileDescriptor(system_handle_type fd, const char* operation, FDType fdType);

  // No copying
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  // Closes the associated descriptor
  void close();

  // Stops tracking the descriptor, returning it to the caller.
  // The caller is then responsible for closing it.
  system_handle_type release();

  // In a boolean context, returns true if this object owns
  // a valid descriptor.
  explicit operator bool() const {
    return fd_ != kInvalid;
  }

  // Returns the underlying descriptor value
  inline system_handle_type system_handle() const {
    return fd_;
  }

#ifndef _WIN32
  // Returns the descriptor value as a file descriptor.
  // This method is only present on posix systems to aid in
  // detecting non-portable use at compile time.
  inline int fd() const {
    return fd_;
  }
#else
  // Returns the descriptor value as a file handle.
  // This method is only present on win32 systems to aid in
  // detecting non-portable use at compile time.
  inline intptr_t handle() const {
    return fd_;
  }
#endif

  inline FDType fdType() const {
    return fdType_;
  }

  // Set the close-on-exec bit
  void setCloExec();

  // Enable non-blocking IO
  void setNonBlock();

  // Disable non-blocking IO
  void clearNonBlock();

  /** equivalent to fstat(2) */
  FileInformation getInfo() const;

  /** Returns the filename associated with the file handle */
  w_string getOpenedPath() const;

  /** Returns the symbolic link target */
  w_string readSymbolicLink() const;

  /** read(2), but yielding a Result for system independent error reporting */
  Result<int, std::error_code> read(void* buf, int size) const;

  /** write(2), but yielding a Result for system independent error reporting */
  Result<int, std::error_code> write(const void* buf, int size) const;

  // Return a global handle to one of the standard IO stream descriptors
  static const FileDescriptor& stdIn();
  static const FileDescriptor& stdOut();
  static const FileDescriptor& stdErr();

 private:
  system_handle_type fd_{kInvalid};
  FDType fdType_{FDType::Unknown};
};
} // namespace watchman
