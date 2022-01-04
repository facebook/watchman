/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/watchman_string.h"
#include "watchman/watchman_system.h"

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
