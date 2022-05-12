/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <system_error>
#include "watchman/portability/WinError.h"
#include "watchman/watchman_string.h"
#include "watchman/watchman_system.h"

bool w_path_exists(const char* path) {
  auto wpath = w_string_piece(path).asWideUNC();
  WIN32_FILE_ATTRIBUTE_DATA data;
  DWORD err;

  if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) {
    err = GetLastError();
    errno = map_win32_err(err);
    return false;
  }
  return true;
}
