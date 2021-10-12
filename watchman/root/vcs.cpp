/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Logging.h"
#include "watchman/root/Root.h"

using namespace watchman;

static json_ref config_get_ignore_vcs(Root* root) {
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

void Root::applyIgnoreVCSConfiguration() {
  auto cookieDirs = cookies.cookieDirs();
  w_check(
      cookieDirs.size() == 1 && cookieDirs.count(root_path) == 1,
      fmt::format(
          "cookie was already initialized with {}: {}\n",
          cookieDirs.size(),
          *cookieDirs.begin()));

  bool default_cookie = true;

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

    if (default_cookie) {
      // While we're at it, see if we can find out where to put our
      // query cookie information
      try {
        auto info = getFileInformation(fullname.c_str(), case_sensitive);
        if (info.isDir()) {
          // root/{.hg,.git,.svn}
          cookies.setCookieDir(fullname);
          default_cookie = false;
        }
      } catch (...) {
        // Don't care
      }
    }
  }
}

/* vim:ts=2:sw=2:et:
 */
