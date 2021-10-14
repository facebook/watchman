/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/fs/FileSystem.h"
#include "watchman/watchman_string.h"

namespace watchman {

#ifdef _WIN32
namespace {
w_string getCurrentDirectory() {
  WCHAR wchar[WATCHMAN_NAME_MAX];
  auto len = GetCurrentDirectoryW(std::size(wchar), wchar);
  auto err = GetLastError();
  // Technically, len > std::size(wchar) is sufficient, because the w_string
  // constructor below will add a trailing zero.
  if (len == 0 || len >= std::size(wchar)) {
    throw std::system_error(
        err, std::system_category(), "GetCurrentDirectoryW");
  }
  // Assumption: that the OS maintains the CWD in canonical form
  return w_string(wchar, len);
}
} // namespace
#endif

w_string realPath(const char* path) {
  auto options = OpenFileHandleOptions::queryFileInfo();
  // Follow symlinks, because that's really the point of this function
  options.followSymlinks = 1;
  options.strictNameChecks = 0;

#ifdef _WIN32
  // Special cases for cwd.
  // On Windows, "" is used to refer to the CWD.
  // We also allow using "." for parity with unix, even though that
  // doesn't generally work for that purpose on windows.
  // This allows `watchman watch-project .` to succeeed on windows.
  if (path[0] == 0 || (path[0] == '.' && path[1] == 0)) {
    return getCurrentDirectory();
  }
#endif

  auto handle = openFileHandle(path, options);
  return handle.getOpenedPath();
}

} // namespace watchman
