/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <cstdint>
#include "FileInformation.h"
#include "watchman_string.h"

namespace watchman {

// Manages the lifetime of a Win32 HANDLE object.
// It will be CloseHandle()d when it is destroyed.
// We track the HANDLE value as intptr_t to avoid needing
// to pull in the windows header files all over the place;
// this is consistent with the _get_osfhandle function in
// the msvcrt library.

class Win32Handle {
  intptr_t h_{0};

 public:
  ~Win32Handle();

  // Default construct to an empty instance
  Win32Handle() = default;

  // Construct a handle object from a handle.
  // Will happily accept an invalid handle with no error.
  explicit Win32Handle(intptr_t h);

  // No copying
  Win32Handle(const Win32Handle&) = delete;
  Win32Handle& operator=(const Win32Handle&) = delete;

  Win32Handle(Win32Handle&& other) noexcept;
  Win32Handle& operator=(Win32Handle&& other) noexcept;

  // Closes the associated Handle
  void close();

  // Stops tracking the Handle, returning it to the caller.
  // The caller is then responsible for closing it.
  intptr_t release();

  // In a boolean context, returns true if this object owns
  // a valid Handle.
  explicit operator bool() const {
    return h_ != 0;
  }

  // Returns the Handle value
  inline intptr_t handle() const {
    return h_;
  }

  /** equivalent to fstat(2) */
  FileInformation getInfo() const;

  /** Returns the filename associated with the file handle */
  w_string getOpenedPath() const;

  /** Returns the symbolic link target */
  w_string readSymbolicLink() const;
};
}
