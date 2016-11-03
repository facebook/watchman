/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

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

bool apply_ignore_vcs_configuration(w_root_t *root, char **errmsg) {
  uint8_t i;
  struct stat st;

  auto ignores = config_get_ignore_vcs(root);
  if (!ignores) {
    ignore_result(asprintf(errmsg, "ignore_vcs must be an array of strings"));
    return false;
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    const json_t *jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      ignore_result(asprintf(errmsg,
          "ignore_vcs must be an array of strings"));
      return false;
    }

    auto fullname =
        w_string::pathCat({root->root_path, json_to_w_string(jignore)});

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (root->ignore.isIgnoreDir(fullname)) {
      continue;
    }

    root->ignore.add(fullname, true);

    // While we're at it, see if we can find out where to put our
    // query cookie information
    if (root->cookies.cookieDir() == root->root_path &&
        w_lstat(fullname.c_str(), &st, root->case_sensitive) == 0 &&
        S_ISDIR(st.st_mode)) {
      // root/{.hg,.git,.svn}
      root->cookies.setCookieDir(fullname);
    }
  }

  return true;
}

/* vim:ts=2:sw=2:et:
 */
