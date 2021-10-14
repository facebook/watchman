/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "watchman/Result.h"
#include "watchman/fs/DirHandle.h"
#include "watchman/fs/FileDescriptor.h"
#include "watchman/fs/FileInformation.h"

/** This header defines platform independent helper functions for
 * operating on the filesystem at a low level.
 * These functions are intended to be used to query information from
 * the filesystem, rather than implementing a full-fledged abstraction
 * for general purpose use.
 *
 * One of the primary features of this header is to provide an OS-Independent
 * alias for the OS-Dependent file descriptor type.
 *
 * The functions in this file generally return or operate on an instance of
 * that type.
 */

namespace watchman {

class FileSystem {
 public:
  virtual ~FileSystem() = default;

  virtual std::unique_ptr<DirHandle> openDir(
      const char* path,
      bool strict = true) = 0;
};

extern FileSystem& realFileSystem;

/** equivalent to open(2)
 * This function is not intended to be used to create files,
 * just to open a file handle to query its metadata */
FileDescriptor openFileHandle(
    const char* path,
    const OpenFileHandleOptions& opts);

/** equivalent to lstat(2), but performs strict name checking */
FileInformation getFileInformation(
    const char* path,
    CaseSensitivity caseSensitive = CaseSensitivity::Unknown);

/** equivalent to realpath() */
w_string realPath(const char* path);

/** equivalent to readlink() */
w_string readSymbolicLink(const char* path);

} // namespace watchman

#ifdef _WIN32
int mkdir(const char* path, int);
#endif
