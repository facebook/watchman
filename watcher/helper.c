/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool w_check_ignores(w_ht_t *ignores, const char *path, uint32_t pathlen) {
  w_ht_iter_t i;

  if (w_ht_first(ignores, &i)) do {
    w_string_t *ign = w_ht_val_ptr(i.value);

    if (pathlen < ign->len) {
      continue;
    }

    if (memcmp(ign->buf, path, ign->len) == 0) {
      if (ign->len == pathlen) {
        // Exact match
        return true;
      }

      if (path[ign->len] == WATCHMAN_DIR_SEP) {
        // prefix match
        return true;
      }
    }

  } while (w_ht_next(ignores, &i));

  return false;
}

// The ignore logic is to stop recursion of grandchildren or later
// generations than an ignored dir.  We allow the direct children
// of an ignored dir, but no further down.
bool w_check_vcs_ignores(w_ht_t *ignores, const char *path, uint32_t pathlen) {
  w_ht_iter_t i;

  if (w_ht_first(ignores, &i)) do {
    w_string_t *ign = w_ht_val_ptr(i.value);

    if (pathlen < ign->len) {
      continue;
    }

    if (memcmp(ign->buf, path, ign->len) == 0) {
      // prefix matches, but it isn't a parent
      if (path[ign->len] != WATCHMAN_DIR_SEP) {
        continue;
      }

      // If we find any '/' in the remainder of the path, then we should
      // ignore it.  Otherwise we allow it.
      path += ign->len + 1;
      pathlen -= ign->len + 1;
      if (memchr(path, WATCHMAN_DIR_SEP, pathlen)) {
        return true;
      }
    }

  } while (w_ht_next(ignores, &i));

  return false;
}

bool w_is_ignored(w_root_t *root, const char *path, uint32_t pathlen)
{
  if (w_check_ignores(root->ignore.ignore_dirs, path, pathlen)) {
    return true;
  }

  return w_check_vcs_ignores(root->ignore.ignore_vcs, path, pathlen);
}
