/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman.h"

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
