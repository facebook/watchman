/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// The path and everything below it is ignored.
#define FULL_IGNORE  (void*)0x1
// The grand-children of the path are ignored, but not the path
// or its direct children.
#define VCS_IGNORE   (void*)0x2

bool w_ignore_init(struct watchman_ignore *ignore) {
  ignore->ignore_vcs = w_ht_new(2, &w_ht_string_funcs);
  ignore->dirs_vec = NULL;
  if (!ignore->ignore_vcs) {
    return false;
  }
  ignore->ignore_dirs = w_ht_new(2, &w_ht_string_funcs);
  if (!ignore->ignore_dirs) {
    return false;
  }
  if (art_tree_init(&ignore->tree) != 0) {
    return false;
  }
  return true;
}

void w_ignore_addstr(struct watchman_ignore *ignore, w_string_t *path,
                     bool is_vcs_ignore) {
  w_ht_set(is_vcs_ignore ? ignore->ignore_vcs : ignore->ignore_dirs,
           w_ht_ptr_val(path), w_ht_ptr_val(path));

  art_insert(&ignore->tree, (const unsigned char *)path->buf, path->len,
             is_vcs_ignore ? VCS_IGNORE : FULL_IGNORE);

  if (!is_vcs_ignore) {
    ignore->dirs_vec = (w_string_t**)realloc(
        ignore->dirs_vec, w_ht_size(ignore->ignore_dirs) * sizeof(w_string_t*));
    if (!ignore->dirs_vec) {
      w_log(W_LOG_FATAL, "OOM while recording ignore dirs");
    }

    // No need to add a ref, as that is tracked by the hash table
    ignore->dirs_vec[w_ht_size(ignore->ignore_dirs)-1] = path;
  }
}

bool w_ignore_check(const struct watchman_ignore *ignore, const char *path,
                    uint32_t pathlen) {
  const char *skip_prefix;
  uint32_t len;
  art_leaf *leaf = art_longest_match(&ignore->tree, (const unsigned char *)path,
                                     (int)pathlen);

  if (!leaf) {
    // No entry -> not ignored.
    return false;
  }

  if (pathlen < leaf->key_len) {
    // We wanted "buil" but matched "build"
    return false;
  }

  if (pathlen == leaf->key_len) {
    // Exact match.  This is an ignore if we are in FULL_IGNORE,
    // but not in VCS_IGNORE mode.
    return leaf->value == FULL_IGNORE ? true : false;
  }

  // Our input string was longer than the leaf key string.
  // We need to ensure that we observe a directory separator at the
  // character after the common prefix, otherwise we may be falsely
  // matching a sibling entry.
  skip_prefix = path + leaf->key_len;
  len = pathlen - leaf->key_len;

  if (*skip_prefix != WATCHMAN_DIR_SEP
#ifdef _WIN32
      // On windows, both '/' and '\' are possible
      && *skip_prefix != '/'
#endif
      ) {
    // we wanted "foo/bar" but we matched something like "food"
    // this is not an ignore situation.
    return false;
  }

  if (leaf->value == FULL_IGNORE) {
    // Definitely ignoring this portion of the tree
    return true;
  }

  // we need to apply vcs_ignore style logic to determine if we are ignoring
  // this path.  This devolves to: "is there a '/' character after the end of
  // the leaf key prefix?"

  if (pathlen <= leaf->key_len) {
    // There can't be a slash after this portion of the tree, therefore
    // this is not ignored.
    return false;
  }

  // Skip over the '/'
  skip_prefix++;
  len--;

#ifndef _WIN32
  // If we find a '/' from this point, we are ignoring this path.
  return memchr(skip_prefix, WATCHMAN_DIR_SEP, len) != NULL;
#else
  // On windows, both '/' and '\' are possible.
  while (len > 0) {
    if (*skip_prefix == WATCHMAN_DIR_SEP || *skip_prefix == '/') {
      return true;
    }
    skip_prefix++;
    len--;
  }
  return false;
#endif
}

void w_ignore_destroy(struct watchman_ignore *ignore) {
  w_ht_free(ignore->ignore_vcs);
  ignore->ignore_vcs = NULL;

  w_ht_free(ignore->ignore_dirs);
  ignore->ignore_dirs = NULL;

  free(ignore->dirs_vec);
  ignore->dirs_vec = NULL;

  art_tree_destroy(&ignore->tree);
}

/* vim:ts=2:sw=2:et:
 */
