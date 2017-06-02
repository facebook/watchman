/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
using watchman::CaseSensitivity;

static json_ref config_get_ignore_vcs(w_root_t* root) {
  json_ref ignores = root->config.get("ignore_vcs");
  if (ignores && !json_is_array(ignores)) {
    return nullptr;
  }

  if (!ignores) {
    // default to a well-known set of vcs's
    ignores = json_array({typed_string_to_json(".git"),
                          typed_string_to_json(".svn"),
                          typed_string_to_json(".hg")});
  }
  return ignores;
}

void watchman_root::applyIgnoreVCSConfiguration() {
  uint8_t i;

  auto ignores = config_get_ignore_vcs(this);
  if (!ignores) {
    throw std::runtime_error("ignore_vcs must be an array of strings");
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    const json_t *jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      throw std::runtime_error("ignore_vcs must be an array of strings");
    }

    auto fullname = w_string::pathCat({root_path, json_to_w_string(jignore)});

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (ignore.isIgnoreDir(fullname)) {
      continue;
    }

    ignore.add(fullname, true);

    // While we're at it, see if we can find out where to put our
    // query cookie information
    if (cookies.cookieDir() == root_path) {
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
