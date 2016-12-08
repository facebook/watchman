/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
namespace watchman {

// Manages the lifetime of a file descriptor.
// It will close() the descriptor when it is destroyed.
class FileDescriptor {
  int fd_{-1};

 public:
  ~FileDescriptor();

  // Default construct to an empty instance
  FileDescriptor() = default;

  // Construct a file descriptor object from an fd.
  // Will happily accept an fd of value -1 with no error.
  explicit FileDescriptor(int fd);

  // Construct a file descriptor object from an fd.
  // If fd is -1 will throw a system error with a message
  // constructed from the provided operation name.
  FileDescriptor(int fd, const char* operation);

  // No copying
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  // Closes the associated descriptor
  void close();

  // Stops tracking the descriptor, returning it to the caller.
  // The caller is then responsible for closing it.
  int release();

  // In a boolean context, returns true if this object owns
  // a valid descriptor.
  explicit operator bool() const {
    return fd_ != -1;
  }

  // Returns the descriptor value
  inline int fd() const {
    return fd_;
  }

  // Set the close-on-exec bit
  void setCloExec();

  // Enable non-blocking IO
  void setNonBlock();

  // Disable non-blocking IO
  void clearNonBlock();

  // Returns true if the FD is in non-blocking mode
  bool isNonBlock() const;
};
}
