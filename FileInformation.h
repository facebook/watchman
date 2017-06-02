/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"
#include <sys/stat.h>

namespace watchman {

#ifdef _WIN32
using mode_t = int;
using dev_t = int;
using gid_t = int;
using uid_t = int;
using ino_t = unsigned int;
using nlink_t = unsigned int;
#endif

struct FileInformation {
  // On POSIX systems, the complete mode information.
  // On Windows, this is lossy wrt. symlink information,
  // so it is preferable to use isSymlink() rather than
  // S_ISLNK() on the mode value.
  mode_t mode{0};
  off_t size{0};

  // On Windows systems, these fields are approximated
  // from cheaply available information in a way that is
  // consistent with msvcrt which is widely used by many
  // native win32 applications (including python).
  uid_t uid{0};
  gid_t gid{0};
  ino_t ino{0};
  dev_t dev{0};
  nlink_t nlink{0};

#ifdef _WIN32
  uint32_t fileAttributes{0};
#endif

  struct timespec atime {
    0, 0
  };
  struct timespec mtime {
    0, 0
  };
  struct timespec ctime {
    0, 0
  };

  // Returns true if this file information references
  // a symlink, false otherwise.
  bool isSymlink() const;

  // Returns true if this file information references
  // a directory, false otherwise.
  bool isDir() const;

  // Returns true if this file information references
  // a regular file, false otherwise.
  bool isFile() const;

#ifndef _WIN32
  explicit FileInformation(const struct stat& st);
#else
  // Partially initialize the common fields.
  // There are a number of different forms of windows specific data
  // types that hold the rest of the information and we don't want
  // to pollute the headers with them, so those are populated
  // externally by the APIs declared elsewhere in this header file.
  explicit FileInformation(uint32_t dwFileAttributes);
#endif
  FileInformation() = default;
};
}
