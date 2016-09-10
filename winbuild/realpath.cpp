/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

char *realpath(const char *filename, char *target) {
#if 1 /* Requires Vista or later */
  WCHAR final_buf[WATCHMAN_NAME_MAX];
  WCHAR *final_bufptr = final_buf;
  DWORD err, len;
  char *utf8;
  char *func_name;

  if (target) {
    // The only sane way is to set target = NULL
    w_log(W_LOG_FATAL, "realpath called with target!=NULL");
  }

  int filename_len = (int)strlen(filename);
  if (filename_len == 0) {
    // Special case for "" -> cwd
    func_name = "GetCurrentDirectoryW";
    len = GetCurrentDirectoryW(sizeof(final_buf)/sizeof(final_buf[0]),
            final_buf);
    err = GetLastError();
  } else {
    WCHAR *wfilename;
    wfilename = w_utf8_to_win_unc(filename, filename_len);

    if (!wfilename) {
      w_log(W_LOG_ERR, "failed to convert %s to WCHAR\n", filename);
      return NULL;
    }

    func_name = "CreateFileW";
    HANDLE h = CreateFileW(wfilename, 0 /* query metadata */,
        FILE_SHARE_DELETE|FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    err = GetLastError();

    if (h != INVALID_HANDLE_VALUE) {
      func_name = "GetFinalPathNameByHandleW";
      len = GetFinalPathNameByHandleW(h, final_buf,
          sizeof(final_buf)/sizeof(WCHAR),
          FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
      err = GetLastError();

      if (len >= sizeof(final_buf)/sizeof(WCHAR)) {
        // Repeat with a heap buffer
        final_bufptr = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
        if (final_bufptr == NULL) {
          len = 0;
        } else {
          len = GetFinalPathNameByHandleW(h, final_buf, len,
            FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
          err = GetLastError();
        }
      }
      CloseHandle(h);
    } else {
      len = 0;
    }
    free(wfilename);
  }

  if (len == 0) {
    if (final_bufptr != final_buf) {
      free(final_bufptr);
    }
    w_log(W_LOG_ERR, "(realpath) %s: %s %s\n", func_name,
        filename, win32_strerror(err));
    errno = map_win32_err(err);
    return NULL;
  }

  utf8 = w_win_unc_to_utf8(final_bufptr, len, NULL);
  if (final_bufptr != final_buf) {
    free(final_bufptr);
  }
  return utf8;
#else
  char full_buf[WATCHMAN_NAME_MAX];
  char long_buf[WATCHMAN_NAME_MAX];
  char *base = NULL;
  DWORD full_len, long_len;

  printf("realpath called with '%s'\n", filename);

  if (!strlen(filename)) {
    // Special case for "" -> cwd
    full_len = GetCurrentDirectory(sizeof(full_buf), full_buf);
  } else {
    full_len = GetFullPathName(filename, sizeof(full_buf), full_buf, &base);
  }
  if (full_len > sizeof(full_buf)-1) {
    w_log(W_LOG_FATAL, "GetFullPathName needs %lu chars\n", full_len);
  }

  full_buf[full_len] = 0;
  printf("full: %s\n", full_buf);

  long_len = GetLongPathName(full_buf, long_buf, sizeof(long_buf));
  if (long_len > sizeof(long_buf)-1) {
    w_log(W_LOG_FATAL, "GetLongPathName needs %lu chars\n", long_len);
  }

  long_buf[long_len] = 0;
  printf("long: %s\n", long_buf);

  if (target) {
    // Pray they passed in a big enough buffer
    strcpy(target, long_buf);
    return target;
  }
  return strdup(long_buf);
#endif
}


/* vim:ts=2:sw=2:et:
 */
