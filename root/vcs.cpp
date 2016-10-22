/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

bool vcs_file_exists(
    struct read_locked_watchman_root* lock,
    const char* dname,
    const char* fname) {
  struct watchman_file *file;

  auto dir_name = w_string::pathCat({lock->root->root_path, dname});
  auto view =
      dynamic_cast<watchman::InMemoryView*>(lock->root->inner.view.get());

  if (!view) {
    // TODO: figure out how we're going to handle this for eden;
    // perhaps just bite the bullet and look at the VFS directly?
    return false;
  }

  const auto dir = view->resolveDir(dir_name);

  if (!dir) {
    return false;
  }

  w_string file_name(fname);
  file = dir->getChildFile(file_name);

  if (!file) {
    return false;
  }

  return file->exists;
}

bool is_vcs_op_in_progress(struct read_locked_watchman_root* lock) {
  return vcs_file_exists(lock, ".hg", "wlock") ||
         vcs_file_exists(lock, ".git", "index.lock");
}

static json_ref config_get_ignore_vcs(w_root_t* root) {
  json_ref ignores = root->config.get("ignore_vcs");
  if (ignores && !json_is_array(ignores)) {
    return nullptr;
  }

  if (!ignores) {
    // default to a well-known set of vcs's
    ignores = json_pack("[sss]", ".git", ".svn", ".hg");
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
    if (w_ht_get(root->ignore.ignore_dirs, w_ht_ptr_val(fullname))) {
      continue;
    }

    w_ignore_addstr(&root->ignore, fullname, true);

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
