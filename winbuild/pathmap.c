/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Filename mapping and handling strategy
// We'll track the utf-8 rendition of the underlying filesystem names
// in the watchman datastructures.  We'll convert to Wide Char at the
// boundary with the Windows API.  All paths that we observe from the
// Windows API will be converted to UTF-8.
// TODO: we should use wmain to guarantee that we only ever see UTF-8
// for our program arguments and environment

#define UNC_PREFIX L"\\\\?\\"
#define UNC_PREFIX_LEN 4

char *w_win_unc_to_utf8(WCHAR *wpath, int pathlen) {
  int len, res;
  char *buf;

  if (pathlen == -1) {
    pathlen = (int)wcslen(wpath);
  }
  if (!wcsncmp(wpath, UNC_PREFIX, UNC_PREFIX_LEN)) {
    wpath += UNC_PREFIX_LEN;
    pathlen -= UNC_PREFIX_LEN;
  }

  len = WideCharToMultiByte(CP_UTF8, 0, wpath, pathlen, NULL, 0, NULL, NULL);
  if (len <= 0) {
    errno = map_win32_err(GetLastError());
    return NULL;
  }

  buf = malloc(len + 1);
  if (!buf) {
    return NULL;
  }

  res = WideCharToMultiByte(CP_UTF8, 0, wpath, pathlen, buf, len, NULL, NULL);
  if (res != len) {
    // Weird!
    DWORD err = GetLastError();
    free(buf);
    errno = map_win32_err(err);
    return NULL;
  }

  buf[res] = 0;
  return buf;
}

WCHAR *w_utf8_to_win_unc(const char *path, int pathlen) {
  WCHAR *buf;
  int len, res, i;

  if (pathlen == -1) {
    pathlen = (int)strlen(path);
  }

  if (pathlen == 0) {
    return wcsdup(L"");
  }

  // Step 1, measure up
  len = MultiByteToWideChar(CP_UTF8, 0, path, pathlen, NULL, 0);
  if (len <= 0) {
    w_log(W_LOG_ERR, "utf->unc failed to measure up: %s "
        "pathlen=%d len=%d err=%s\n", path, pathlen, len,
        win32_strerror(GetLastError()));
    errno = map_win32_err(GetLastError());
    return NULL;
  }

  // Step 2, allocate and prepend UNC prefix
  buf = malloc((UNC_PREFIX_LEN + len + 1) * sizeof(WCHAR));
  if (!buf) {
    return NULL;
  }
  wcscpy(buf, UNC_PREFIX);

  // Step 3, convert into the new space
  res = MultiByteToWideChar(CP_UTF8, 0, path, pathlen,
      buf + UNC_PREFIX_LEN, len);

  if (res != len) {
    DWORD err = GetLastError();
    // Something crazy happened
    free(buf);
    errno = map_win32_err(err);
    w_log(W_LOG_ERR, "MultiByteToWideChar: res=%d and len=%d.  wat. %s\n",
        res, len, win32_strerror(err));
    return NULL;
  }

  // Replace all forward slashes with backslashes
  for (i = UNC_PREFIX_LEN; i < UNC_PREFIX_LEN + len; i++) {
    if (buf[i] == '/') {
      buf[i] = '\\';
    }
  }

  buf[UNC_PREFIX_LEN + len] = 0;
  return buf;
}

/* vim:ts=2:sw=2:et:
 */
