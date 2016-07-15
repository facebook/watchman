/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_IGNORE_H
#define WATCHMAN_IGNORE_H

#include "thirdparty/libart/src/art.h"

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_ignore {
  /* map of dir name => dirname
   * if the map has an entry for a given dir, we're ignoring it */
  w_ht_t *ignore_vcs;
  w_ht_t *ignore_dirs;
  /* radix tree containing the same information as the ignore
   * entries above.  This is used only on OS X and Windows because
   * we cannot exclude these dirs using the kernel watching APIs */
  art_tree tree;
  /* On OS X, we need to preserve the order of the ignore list so
   * that we can exclude things deterministically and fit within
   * system limits. */
  w_string_t **dirs_vec;
};

// Initialize ignore state
bool w_ignore_init(struct watchman_ignore *ignore);

// Adds a string to the ignore list.
// The is_vcs_ignore parameter indicates whether it is a full ignore
// or a vcs-style grandchild ignore.
void w_ignore_addstr(struct watchman_ignore *ignore, w_string_t *path,
                     bool is_vcs_ignore);

// Tests whether path is ignored.
// Returns true if the path is ignored, false otherwise.
bool w_ignore_check(struct watchman_ignore *ignore, const char *path,
                    uint32_t pathlen);

// Releases ignore state
void w_ignore_destroy(struct watchman_ignore *ignore);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
