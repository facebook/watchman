/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
using watchman::Win32Handle;

int mkdir(const char* path, int) {
  auto wpath = w_string_piece(path).asWideUNC();
  DWORD err;
  BOOL res;

  res = CreateDirectoryW(wpath.c_str(), nullptr);
  err = GetLastError();

  if (res) {
    return 0;
  }
  errno = map_win32_err(err);
  return -1;
}

/** Replace open with a version that enables all sharing flags.
 * This minimizes the chances that we'll encounter a sharing violation
 * while we try to examine a file. */
int open_and_share(const char *path, int flags, ...) {
  int fd;

  auto h = w_handle_open(path, flags);
  if (!h) {
    return -1;
  }

  fd = _open_osfhandle(h.handle(), flags);
  if (fd == -1) {
    return -1;
  }

  // fd now owns it
  h.release();

  return fd;
}

int lstat(const char *path, struct stat *st) {
  FILE_BASIC_INFO binfo;
  FILE_STANDARD_INFO sinfo;
  auto wpath = w_string_piece(path).asWideUNC();
  DWORD err;

  memset(st, 0, sizeof(*st));

  Win32Handle h(intptr_t(CreateFileW(
      wpath.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr)));
  err = GetLastError();

  if (!h) {
    w_log(W_LOG_DBG, "lstat(%s): %s\n", path, win32_strerror(err));
    errno = map_win32_err(err);
    return -1;
  }

  if (path[1] == ':') {
    int drive_letter = tolower(path[0]);
    st->st_rdev = st->st_dev = drive_letter - 'a';
  }

  if (GetFileInformationByHandleEx(
          (HANDLE)h.handle(), FileBasicInfo, &binfo, sizeof(binfo))) {
    FILETIME_LARGE_INTEGER_to_timespec(binfo.CreationTime, &st->st_ctim);
    st->st_ctime = st->st_ctim.tv_sec;
    FILETIME_LARGE_INTEGER_to_timespec(binfo.LastAccessTime, &st->st_atim);
    st->st_atime = st->st_atim.tv_sec;
    FILETIME_LARGE_INTEGER_to_timespec(binfo.LastWriteTime, &st->st_mtim);
    st->st_mtime = st->st_mtim.tv_sec;

    if (binfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // This is a symlink, but msvcrt has no way to indicate that.
      // We'll treat it as a regular file until we have a better
      // representation :-/
      st->st_mode = _S_IFREG;
    } else if (binfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      st->st_mode |= _S_IFDIR|S_IEXEC|S_IXGRP|S_IXOTH;
    } else {
      st->st_mode |= _S_IFREG;
    }
    if (binfo.FileAttributes & FILE_ATTRIBUTE_READONLY) {
      st->st_mode |= 0444;
    } else {
      st->st_mode |= 0666;
    }
  }

  if (GetFileInformationByHandleEx(
          (HANDLE)h.handle(), FileStandardInfo, &sinfo, sizeof(sinfo))) {
    st->st_size = sinfo.EndOfFile.QuadPart;
    st->st_nlink = sinfo.NumberOfLinks;
  }

  return 0;
}

/* vim:ts=2:sw=2:et:
 */
