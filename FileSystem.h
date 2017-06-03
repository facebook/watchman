/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "FileInformation.h"
#include "FileDescriptor.h"
#include "Result.h"

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

enum class CaseSensitivity {
  // The caller knows that the filesystem path(s) in question are
  // case insensitive.
  CaseInSensitive,
  // The caller knows that the filesystem path(s) in question are
  // case sensitive.
  CaseSensitive,
  // The caller does not know if the path(s) are case sensitive
  Unknown,
};

/** Returns CaseSensitive or CaseInSensitive depending on the
 * case sensitivity of the input path. */
CaseSensitivity getCaseSensitivityForPath(const char *path);

/** Windows doesn't have equivalent bits for all of the various
 * open(2) flags, so we abstract it out here */
struct OpenFileHandleOptions {
  unsigned followSymlinks : 1;   // O_NOFOLLOW
  unsigned closeOnExec : 1;      // O_CLOEXEC
  unsigned metaDataOnly : 1;     // avoid accessing file contents
  unsigned readContents : 1;     // the read portion of O_RDONLY or O_RDWR
  unsigned writeContents : 1;    // the write portion of O_WRONLY or O_RDWR
  unsigned create : 1;           // O_CREAT
  unsigned exclusiveCreate : 1;  // O_EXCL
  unsigned truncate : 1;         // O_TRUNC
  unsigned strictNameChecks : 1;
  CaseSensitivity caseSensitive;

  OpenFileHandleOptions()
      : followSymlinks(0), closeOnExec(1), metaDataOnly(0), readContents(0),
        writeContents(0), create(0), exclusiveCreate(0), truncate(0),
        strictNameChecks(1), caseSensitive(CaseSensitivity::Unknown) {}

  static inline OpenFileHandleOptions queryFileInfo() {
    OpenFileHandleOptions opts;
    opts.metaDataOnly = 1;
    return opts;
  }

  static inline OpenFileHandleOptions openDir() {
    OpenFileHandleOptions opts;
    opts.readContents = 1;
    opts.strictNameChecks = false;
    opts.followSymlinks = 1;
    return opts;
  }

  static inline OpenFileHandleOptions strictOpenDir() {
    OpenFileHandleOptions opts;
    opts.readContents = 1;
    opts.strictNameChecks = true;
    opts.followSymlinks = 0;
    return opts;
  }
};

/** equivalent to open(2)
 * This function is not intended to be used to create files,
 * just to open a file handle to query its metadata */
FileDescriptor openFileHandle(
    const char* path,
    const OpenFileHandleOptions& opts);

/** equivalent to lstat(2), but performs strict name checking */
FileInformation
getFileInformation(const char *path,
                   CaseSensitivity caseSensitive = CaseSensitivity::Unknown);

/** equivalent to realpath() */
w_string realPath(const char *path);

/** equivalent to readlink() */
w_string readSymbolicLink(const char* path);
}

#ifdef _WIN32
int mkdir(const char* path, int);
#endif
