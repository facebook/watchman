/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "Win32Handle.h"
using watchman::Win32Handle;

char *realpath(const char *filename, char *target) {
  std::wstring wchar;
  DWORD err, len;
  char *utf8;
  char *func_name;

  wchar.resize(WATCHMAN_NAME_MAX);

  if (target) {
    // The only sane way is to set target = NULL
    w_log(W_LOG_FATAL, "realpath called with target!=NULL");
  }

  int filename_len = (int)strlen(filename);
  if (filename_len == 0) {
    // Special case for "" -> cwd
    func_name = "GetCurrentDirectoryW";
    len = GetCurrentDirectoryW(wchar.size(), &wchar[0]);
    err = GetLastError();
  } else {
    auto wfilename = w_string_piece(filename, filename_len).asWideUNC();

    func_name = "CreateFileW";
    Win32Handle h(intptr_t(CreateFileW(
        wfilename.c_str(),
        0 /* query metadata */,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)));
    err = GetLastError();

    if (h) {
      func_name = "GetFinalPathNameByHandleW";
      len = GetFinalPathNameByHandleW(
          (HANDLE)h.handle(),
          &wchar[0],
          wchar.size(),
          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      err = GetLastError();

      if (len >= wchar.size()) {
        // Grow it
        wchar.resize(len);
        len = GetFinalPathNameByHandleW(
            (HANDLE)h.handle(),
            &wchar[0],
            len,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        err = GetLastError();
      }
    } else {
      len = 0;
    }
  }

  if (len == 0) {
    errno = map_win32_err(err);
    return nullptr;
  }

  w_string result(wchar.data(), len);
  return strdup(result.c_str());
}


/* vim:ts=2:sw=2:et:
 */
