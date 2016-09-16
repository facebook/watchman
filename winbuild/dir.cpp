/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

DIR *win_opendir(const char *path, int nofollow) {
  struct watchman_win32_dir *d = NULL;
  WCHAR *wpath = NULL;
  int err = 0;

  d = (watchman_win32_dir*)calloc(1, sizeof(*d));
  if (!d) {
    err = errno;
    goto err;
  }

  wpath = w_utf8_to_win_unc(path, -1);
  if (!path) {
    err = errno;
    goto err;
  }

  d->h = CreateFileW(wpath, GENERIC_READ,
      FILE_SHARE_DELETE|FILE_SHARE_READ|FILE_SHARE_WRITE,
      NULL,
      OPEN_EXISTING,
      (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0)|
      FILE_FLAG_BACKUP_SEMANTICS,
      NULL);

  if (d->h == INVALID_HANDLE_VALUE) {
    err = map_win32_err(GetLastError());
    goto err;
  }

  free(wpath);
  return d;

err:
  free(wpath);
  free(d);
  errno = err;
  return NULL;
}

DIR *opendir(const char *path) {
  return win_opendir(path, 0);
}

struct dirent *readdir(DIR *d) {
  if (!d->info) {
    if (!GetFileInformationByHandleEx(d->h, FileFullDirectoryInfo,
          d->buf, sizeof(d->buf))) {
      errno = map_win32_err(GetLastError());
      return NULL;
    }
    d->info = (FILE_FULL_DIR_INFO*)d->buf;
  }

  // Decode the item currently pointed at
  DWORD len = WideCharToMultiByte(CP_UTF8, 0, d->info->FileName,
      d->info->FileNameLength / sizeof(WCHAR), d->ent.d_name,
      sizeof(d->ent.d_name)-1,
      NULL, NULL);

  if (len <= 0) {
    errno = map_win32_err(GetLastError());
    return NULL;
  }
  d->ent.d_name[len] = 0;

  // Advance the pointer to the next entry ready for the next read
  d->info = d->info->NextEntryOffset == 0 ? NULL :
    (FILE_FULL_DIR_INFO*)(((char*)d->info) + d->info->NextEntryOffset);
  return &d->ent;
}

void closedir(DIR *d) {
  CloseHandle(d->h);
  free(d);
}
