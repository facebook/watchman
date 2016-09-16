/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_file {
  /* the parent dir */
  struct watchman_dir *parent;

  /* linkage to files ordered by changed time */
  struct watchman_file *prev, *next;

  /* linkage to files ordered by common suffix */
  struct watchman_file *suffix_prev, *suffix_next;

  /* the time we last observed a change to this file */
  w_clock_t otime;
  /* the time we first observed this file OR the time
   * that this file switched from !exists to exists.
   * This is thus the "created time" */
  w_clock_t ctime;

  /* whether we believe that this file still exists */
  bool exists;
  /* whether we think this file might not exist */
  bool maybe_deleted;

  /* cache stat results so we can tell if an entry
   * changed */
  struct watchman_stat stat;

  /* the symbolic link target of this file.
   * Can be NULL if not a symlink, or we failed to read the target */
  w_string_t *symlink_target;
};

static inline w_string_t *w_file_get_name(struct watchman_file *file) {
  return (w_string_t*)(file + 1);
}
