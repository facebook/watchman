/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

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

/* vim:ts=2:sw=2:et:
 */
