/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_dir {
  /* the name of this dir, relative to its parent */
  w_string_t *name;
  /* the parent dir */
  struct watchman_dir *parent;

  /* files contained in this dir (keyed by file->name) */
  w_ht_t *files;
  /* child dirs contained in this dir (keyed by dir->path) */
  w_ht_t *dirs;
  // If we think this dir was deleted, we'll avoid recursing
  // to its children when processing deletes
  bool last_check_existed;
};
