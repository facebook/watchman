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
