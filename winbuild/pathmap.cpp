/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <algorithm>

// Filename mapping and handling strategy
// We'll track the utf-8 rendition of the underlying filesystem names
// in the watchman datastructures.  We'll convert to Wide Char at the
// boundary with the Windows API.  All paths that we observe from the
// Windows API will be converted to UTF-8.
// TODO: we should use wmain to guarantee that we only ever see UTF-8
// for our program arguments and environment

#define LEN_ESCAPE L"\\\\?\\"
#define LEN_ESCAPE_LEN 4
#define UNC_PREFIX L"UNC"
#define UNC_PREFIX_LEN 3
// We see this escape come back from reparse points when reading
// junctions/symlinks
#define SYMLINK_ESCAPE L"\\??\\"
#define SYMLINK_ESCAPE_LEN 4

w_string::w_string(const WCHAR* wpath, size_t pathlen) {
  int len, res;
  bool is_unc = false;

  if (!wcsncmp(wpath, SYMLINK_ESCAPE, SYMLINK_ESCAPE_LEN)) {
    wpath += SYMLINK_ESCAPE_LEN;
    pathlen -= SYMLINK_ESCAPE_LEN;
  }

  if (!wcsncmp(wpath, LEN_ESCAPE, LEN_ESCAPE_LEN)) {
    wpath += LEN_ESCAPE_LEN;
    pathlen -= LEN_ESCAPE_LEN;

    if (pathlen >= UNC_PREFIX_LEN + 1 &&
        !wcsncmp(wpath, UNC_PREFIX, UNC_PREFIX_LEN) &&
        wpath[UNC_PREFIX_LEN] == '\\') {
      // Need to convert "\\?\UNC\server\share" to "\\server\share"
      // We'll pass "C\server\share" and then poke a "\" in at the
      // start
      wpath += UNC_PREFIX_LEN - 1;
      pathlen -= (UNC_PREFIX_LEN - 1);
      is_unc = true;
    }
  }

  len = WideCharToMultiByte(
      CP_UTF8, 0, wpath, pathlen, nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    throw std::system_error(
        GetLastError(), std::system_category(), "WideCharToMultiByte");
  }

  str_ = (w_string_t*)(new char[sizeof(w_string_t) + len + 1]);
  new (str_) watchman_string();

  str_->refcnt = 1;
  str_->len = len;
  auto buf = (char*)(str_ + 1);
  str_->buf = buf;
  str_->type = W_STRING_UNICODE;

  res = WideCharToMultiByte(
      CP_UTF8, 0, wpath, pathlen, buf, len, nullptr, nullptr);
  if (res != len) {
    // Weird!
    throw std::system_error(
        GetLastError(), std::system_category(), "WideCharToMultiByte");
  }

  if (is_unc) {
    // Replace the "C" from UNC with a leading slash
    buf[0] = '\\';
  }

  // Normalize directory separators for our internal UTF-8
  // strings to be forward slashes.  These will get transformed
  // to backslashes when converting to WCHAR in our counterpart
  // w_string_piece::asWideUNC()
  std::transform(
      buf, buf + len, buf, [](wchar_t c) { return c == '\\' ? '/' : c; });

  buf[res] = 0;
}

bool w_path_exists(const char *path) {
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

std::wstring w_string_piece::asWideUNC() const {
  int len, res, i, prefix_len;
  bool use_escape = false;
  bool is_unc = false;

  // Make a copy of ourselves, as we may mutate
  w_string_piece path = *this;

  if (path.size() == 0) {
    return std::wstring(L"");
  }

  // We don't want to use the length escape for special filenames like NUL:
  // (which we use as the equivalent to /dev/null).
  // We only strictly need to use the escape when pathlen >= MAX_PATH,
  // but since such paths are rare, we want to ensure that we hit any
  // problems with the escape approach on common paths (not all windows
  // API functions apparently support this very well)
  if (path.size() > 3 && path.size() < MAX_PATH &&
      path[path.size() - 1] == ':') {
    use_escape = false;
    prefix_len = 0;
  } else {
    use_escape = true;

    prefix_len = LEN_ESCAPE_LEN;

    // More func with escaped names when UNC notation is used
    if (path[0] == '\\' && path[1] == '\\') {
      is_unc = true;
      prefix_len += UNC_PREFIX_LEN;
    }
  }

  // Step 1, measure up
  len = MultiByteToWideChar(CP_UTF8, 0, path.data(), path.size(), nullptr, 0);
  if (len <= 0) {
    throw std::system_error(
        GetLastError(), std::system_category(), "MultiByteToWideChar");
  }

  // Step 2, allocate and prepend UNC prefix
  std::wstring result;
  result.reserve(prefix_len + len + 1);

  if (use_escape) {
    result.append(LEN_ESCAPE);

    // UNC paths need further mangling when using length escapes
    if (is_unc) {
      result.append(UNC_PREFIX);
      // "\\server\path" -> "\\?\UNC\server\path"
      path = w_string_piece(
          path.data() + 1,
          path.size() - 1); // Skip the first of these two slashes
    }
  }

  // Step 3, convert into the new space
  result.resize(prefix_len + len);
  res = MultiByteToWideChar(
      CP_UTF8, 0, path.data(), path.size(), &result[prefix_len], len);

  if (res != len) {
    // Something crazy happened
    throw std::system_error(
        GetLastError(), std::system_category(), "MultiByteToWideChar");
  }

  // Replace all forward slashes with backslashes.  This makes things easier
  // for clients that are just jamming paths together using /, but is also
  // required when we are using the long filename escape prefix
  for (auto& c : result) {
    if (c == '/') {
      c = '\\';
    }
  }
  return result;
}

/* vim:ts=2:sw=2:et:
 */
