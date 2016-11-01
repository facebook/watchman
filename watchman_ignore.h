/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_IGNORE_H
#define WATCHMAN_IGNORE_H

#include <unordered_set>
#include "thirdparty/libart/src/art.h"

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_ignore {
  /* if the map has an entry for a given dir, we're ignoring it */
  std::unordered_set<w_string> ignore_vcs;
  std::unordered_set<w_string> ignore_dirs;
  /* radix tree containing the same information as the ignore
   * entries above.  This is used only on OS X and Windows because
   * we cannot exclude these dirs using the kernel watching APIs */
  art_tree<uint8_t, w_string> tree;
  /* On OS X, we need to preserve the order of the ignore list so
   * that we can exclude things deterministically and fit within
   * system limits. */
  std::vector<w_string> dirs_vec;

  // Adds a string to the ignore list.
  // The is_vcs_ignore parameter indicates whether it is a full ignore
  // or a vcs-style grandchild ignore.
  void add(const w_string& path, bool is_vcs_ignore);

  // Tests whether path is ignored.
  // Returns true if the path is ignored, false otherwise.
  bool isIgnored(const char* path, uint32_t pathlen) const;

  // Test whether path is listed in ignore vcs config
  bool isIgnoreVCS(const w_string& path) const;

  // Test whether path is listed in ignore dir config
  bool isIgnoreDir(const w_string& path) const;
};


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
