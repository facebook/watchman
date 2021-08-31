/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "watchman/FileInformation.h"

struct watchman_dir_ent {
  bool has_stat;
  char* d_name;
  watchman::FileInformation stat;
};

class watchman_dir_handle {
 public:
  virtual ~watchman_dir_handle() = default;
  virtual const watchman_dir_ent* readDir() = 0;
#ifndef _WIN32
  virtual int getFd() const = 0;
#endif
};

// Return a dir handle on path.
// Does not follow symlinks strict == true.
// Throws std::system_error if the dir could not be opened.
std::unique_ptr<watchman_dir_handle> w_dir_open(
    const char* path,
    bool strict = true);
