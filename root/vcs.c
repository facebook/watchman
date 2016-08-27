/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool vcs_file_exists(struct write_locked_watchman_root *lock, const char *dname,
                     const char *fname) {
  struct watchman_dir *dir;
  struct watchman_file *file;
  w_string_t *file_name;
  w_string_t *dir_name;
  w_string_t *rel_dir_name;

  rel_dir_name = w_string_new_typed(dname, W_STRING_BYTE);
  dir_name = w_string_path_cat(lock->root->root_path, rel_dir_name);
  w_string_delref(rel_dir_name);

  dir = w_root_resolve_dir(lock, dir_name, false);
  w_string_delref(dir_name);

  if (!dir) {
    return false;
  }

  if (!dir->files) {
    return false;
  }

  file_name = w_string_new_typed(fname, W_STRING_BYTE);
  file = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
  w_string_delref(file_name);

  if (!file) {
    return false;
  }

  return file->exists;
}

bool is_vcs_op_in_progress(struct write_locked_watchman_root *lock) {
  return vcs_file_exists(lock, ".hg", "wlock") ||
         vcs_file_exists(lock, ".git", "index.lock");
}

static json_t *config_get_ignore_vcs(w_root_t *root) {
  json_t *ignores = cfg_get_json(root, "ignore_vcs");
  if (ignores && !json_is_array(ignores)) {
    return NULL;
  }

  if (ignores) {
    // incref so that the caller can simply decref whatever we return
    json_incref(ignores);
  } else {
    // default to a well-known set of vcs's
    ignores = json_pack("[sss]", ".git", ".svn", ".hg");
  }
  return ignores;
}

bool apply_ignore_vcs_configuration(w_root_t *root, char **errmsg) {
  w_string_t *name;
  w_string_t *fullname;
  uint8_t i;
  json_t *ignores;
  char hostname[256];
  struct stat st;

  ignores = config_get_ignore_vcs(root);
  if (!ignores) {
    ignore_result(asprintf(errmsg, "ignore_vcs must be an array of strings"));
    return false;
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    const json_t *jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      ignore_result(asprintf(errmsg,
          "ignore_vcs must be an array of strings"));
      json_decref(ignores);
      return false;
    }

    name = json_to_w_string(jignore);
    fullname = w_string_path_cat(root->root_path, name);

    // if we are completely ignoring this dir, we have nothing more to
    // do here
    if (w_ht_get(root->ignore.ignore_dirs, w_ht_ptr_val(fullname))) {
      w_string_delref(fullname);
      continue;
    }

    w_ignore_addstr(&root->ignore, fullname, true);

    // While we're at it, see if we can find out where to put our
    // query cookie information
    if (root->query_cookie_dir == NULL &&
        w_lstat(fullname->buf, &st, root->case_sensitive) == 0 &&
        S_ISDIR(st.st_mode)) {
      // root/{.hg,.git,.svn}
      root->query_cookie_dir = w_string_path_cat(root->root_path, name);
    }
    w_string_delref(fullname);
  }

  json_decref(ignores);

  if (root->query_cookie_dir == NULL) {
    w_string_addref(root->root_path);
    root->query_cookie_dir = root->root_path;
  }
  gethostname(hostname, sizeof(hostname));
  hostname[sizeof(hostname) - 1] = '\0';

  root->query_cookie_prefix = w_string_make_printf(
      "%.*s%c" WATCHMAN_COOKIE_PREFIX "%s-%d-", root->query_cookie_dir->len,
      root->query_cookie_dir->buf, WATCHMAN_DIR_SEP, hostname, (int)getpid());
  return true;
}

/* vim:ts=2:sw=2:et:
 */
