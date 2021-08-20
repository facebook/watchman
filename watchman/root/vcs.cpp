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

#include "watchman/InMemoryView.h"
#include "watchman/watchman_root.h"

static json_ref config_get_ignore_vcs(watchman_root* root) {
  json_ref ignores = root->config.get("ignore_vcs");
  if (ignores && !ignores.isArray()) {
    return nullptr;
  }

  if (!ignores) {
    // default to a well-known set of vcs's
    ignores = json_array(
        {typed_string_to_json(".git"),
         typed_string_to_json(".svn"),
         typed_string_to_json(".hg")});
  }
  return ignores;
}

void watchman_root::applyIgnoreVCSConfiguration() {
  auto ignores = config_get_ignore_vcs(this);
  if (!ignores) {
    throw std::runtime_error("ignore_vcs must be an array of strings");
  }

  for (auto& jignore : ignores.array()) {
    if (!jignore.isString()) {
      throw std::runtime_error("ignore_vcs must be an array of strings");
    }

    auto fullname = w_string::pathCat({root_path, json_to_w_string(jignore)});

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (ignore.isIgnoreDir(fullname)) {
      continue;
    }

    ignore.add(fullname, true);

    // Since we do not have a watcher just yet, we can't test if the watcher
    // will have WATCHER_HAS_SPLIT_WATCH, thus, rely on whether only the root
    // is present in the cookies dirs.
    auto cookieDirs = cookies.cookieDirs();
    if (cookieDirs.size() == 1 && cookieDirs.count(root_path) == 1) {
      // While we're at it, see if we can find out where to put our
      // query cookie information
      try {
        auto info = getFileInformation(fullname.c_str(), case_sensitive);
        if (info.isDir()) {
          // root/{.hg,.git,.svn}
          cookies.setCookieDir(fullname);
        }
      } catch (...) {
        // Don't care
      }
    }
  }
}

/* vim:ts=2:sw=2:et:
 */
