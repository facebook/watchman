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

#define LEN_ESCAPE L"\\\\?\\"
#define LEN_ESCAPE_LEN 4
#define UNC_PREFIX L"UNC"
#define UNC_PREFIX_LEN 3

char *w_win_unc_to_utf8(WCHAR *wpath, int pathlen, uint32_t *outlen) {
  int len, res;
  char *buf;
  bool is_unc = false;

  if (pathlen == -1) {
    pathlen = (int)wcslen(wpath);
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

  len = WideCharToMultiByte(CP_UTF8, 0, wpath, pathlen, NULL, 0, NULL, NULL);
  if (len <= 0) {
    errno = map_win32_err(GetLastError());
    return NULL;
  }

  buf = (char*)malloc(len + 1);
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

  if (is_unc) {
    // Replace the "C" from UNC with a leading slash
    buf[0] = '\\';
  }

  buf[res] = 0;
  if (outlen) {
    *outlen = res;
  }
  return buf;
}

bool w_path_exists(const char *path) {
  WCHAR *wpath = w_utf8_to_win_unc(path, -1);
  WIN32_FILE_ATTRIBUTE_DATA data;
  DWORD err;

  if (!wpath) {
    return false;
  }
  if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &data)) {
    err = GetLastError();
    free(wpath);
    errno = map_win32_err(err);
    return false;
  }
  free(wpath);
  return true;
}

WCHAR *w_utf8_to_win_unc(const char *path, int pathlen) {
  WCHAR *buf, *target;
  int len, res, i, prefix_len;
  bool use_escape = false;
  bool is_unc = false;

  if (pathlen == -1) {
    pathlen = (int)strlen(path);
  }

  if (pathlen == 0) {
    return wcsdup(L"");
  }

  // We don't want to use the length escape for special filenames like NUL:
  // (which we use as the equivalent to /dev/null).
  // We only strictly need to use the escape when pathlen >= MAX_PATH,
  // but since such paths are rare, we want to ensure that we hit any
  // problems with the escape approach on common paths (not all windows
  // API functions apparently support this very well)
  if (pathlen > 3 && pathlen < MAX_PATH && path[pathlen-1] == ':') {
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
  len = MultiByteToWideChar(CP_UTF8, 0, path, pathlen, NULL, 0);
  if (len <= 0) {
    w_log(W_LOG_ERR, "utf->unc failed to measure up: %s "
        "pathlen=%d len=%d err=%s\n", path, pathlen, len,
        win32_strerror(GetLastError()));
    errno = map_win32_err(GetLastError());
    return NULL;
  }

  // Step 2, allocate and prepend UNC prefix
  buf = (WCHAR*)malloc((prefix_len + len + 1) * sizeof(WCHAR));
  if (!buf) {
    return NULL;
  }

  target = buf;
  if (use_escape) {
    wcscpy(buf, LEN_ESCAPE);
    target = buf + LEN_ESCAPE_LEN;

    // UNC paths need further mangling when using length escapes
    if (is_unc) {
      wcscpy(target, UNC_PREFIX);
      target += UNC_PREFIX_LEN;
      // "\\server\path" -> "\\?\UNC\server\path"
      path++; // Skip the first of these two slashes
    }
  }

  // Step 3, convert into the new space
  res = MultiByteToWideChar(CP_UTF8, 0, path, pathlen, target, len);

  if (res != len) {
    DWORD err = GetLastError();
    // Something crazy happened
    free(buf);
    errno = map_win32_err(err);
    w_log(W_LOG_ERR, "MultiByteToWideChar: res=%d and len=%d.  wat. %s\n",
        res, len, win32_strerror(err));
    return NULL;
  }

  // Replace all forward slashes with backslashes.  This makes things easier
  // for clients that are just jamming paths together using /, but is also
  // required when we are using the long filename escape prefix
  for (i = 0; i < len; i++) {
    if (target[i] == '/') {
      target[i] = '\\';
    }
  }
  target[len] = 0;
  return buf;
}

/* vim:ts=2:sw=2:et:
 */
