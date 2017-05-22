/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "FileDescriptor.h"
#include "FileSystem.h"

#include <system_error>

namespace watchman {

FileDescriptor::~FileDescriptor() {
  close();
}

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::FileDescriptor(int fd, const char* operation) : fd_(fd) {
  if (fd_ == -1) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string(operation) + ": " + strerror(errno));
  }
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  close();
  fd_ = other.fd_;
  other.fd_ = -1;
  return *this;
}

void FileDescriptor::close() {
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

int FileDescriptor::release() {
  int result = fd_;
  fd_ = -1;
  return result;
}

void FileDescriptor::setCloExec() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFD, FD_CLOEXEC));
#endif
}

void FileDescriptor::setNonBlock() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK));
#endif
}

void FileDescriptor::clearNonBlock() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) & ~O_NONBLOCK));
#endif
}

bool FileDescriptor::isNonBlock() const {
#ifndef _WIN32
  return (fcntl(fd_, F_GETFL) & O_NONBLOCK) == O_NONBLOCK;
#else
  return false;
#endif
}

#ifndef _WIN32
FileHandleType openFileHandle(const char *path,
                              const OpenFileHandleOptions &opts) {
  int flags = (!opts.followSymlinks ? O_NOFOLLOW : 0) |
              (opts.closeOnExec ? O_CLOEXEC : 0) |
#ifdef O_PATH
              (opts.metaDataOnly ? O_PATH : 0) |
#endif
              ((opts.readContents && opts.writeContents)
                   ? O_RDWR
                   : (opts.writeContents ? O_WRONLY
                                         : opts.readContents ? O_RDONLY : 0)) |
              (opts.create ? O_CREAT : 0) |
              (opts.exclusiveCreate ? O_EXCL : 0) |
              (opts.truncate ? O_TRUNC : 0);

  auto fd = open(path, flags);
  if (fd == -1) {
    int err = errno;
    throw std::system_error(
        err, std::generic_category(), to<std::string>("open: ", path));
  }
  return FileDescriptor(fd);
}

FileInformation FileDescriptor::getInfo() const {
  struct stat st;
  if (fstat(fd_, &st)) {
    int err = errno;
    throw std::system_error(err, std::generic_category(), "fstat");
  }
  return FileInformation(st);
}
#endif

}
