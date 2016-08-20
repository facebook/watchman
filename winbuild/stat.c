/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

int mkdir(const char *path, int mode) {
  WCHAR *wpath = w_utf8_to_win_unc(path, -1);
  DWORD err;
  BOOL res;

  unused_parameter(mode);

  if (!wpath) {
    return -1;
  }

  res = CreateDirectoryW(wpath, NULL);
  err = GetLastError();
  free(wpath);

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
  HANDLE h;
  int fd;

  h = w_handle_open(path, flags);
  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }

  fd = _open_osfhandle((intptr_t)h, flags);

  if (fd == -1) {
    CloseHandle(h);
  }

  return fd;
}

int lstat(const char *path, struct stat *st) {
  FILE_BASIC_INFO binfo;
  FILE_STANDARD_INFO sinfo;
  WCHAR *wpath = w_utf8_to_win_unc(path, -1);
  HANDLE h;
  DWORD err;

  memset(st, 0, sizeof(*st));

  if (!wpath) {
    return -1;
  }

  h = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
        FILE_SHARE_DELETE|FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
  err = GetLastError();
  free(wpath);

  if (h == INVALID_HANDLE_VALUE) {
    w_log(W_LOG_DBG, "lstat(%s): %s\n", path, win32_strerror(err));
    errno = map_win32_err(err);
    return -1;
  }

  if (path[1] == ':') {
    int drive_letter = tolower(path[0]);
    st->st_rdev = st->st_dev = drive_letter - 'a';
  }

  if (GetFileInformationByHandleEx(h, FileBasicInfo, &binfo, sizeof(binfo))) {
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

  if (GetFileInformationByHandleEx(h, FileStandardInfo,
        &sinfo, sizeof(sinfo))) {
    st->st_size = sinfo.EndOfFile.QuadPart;
    st->st_nlink = sinfo.NumberOfLinks;
  }

  CloseHandle(h);

  return 0;
}

/* vim:ts=2:sw=2:et:
 */
