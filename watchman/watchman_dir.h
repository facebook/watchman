/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <unordered_map>
#include "watchman/watchman_string.h"

struct watchman_file;

struct watchman_dir {
  /* the name of this dir, relative to its parent */
  w_string name;
  /* the parent dir */
  watchman_dir* parent;

  /* files contained in this dir (keyed by file->name) */
  struct Deleter {
    void operator()(watchman_file*) const;
  };
  std::unordered_map<w_string_piece, std::unique_ptr<watchman_file, Deleter>>
      files;

  /* child dirs contained in this dir (keyed by dir->name) */
  std::unordered_map<w_string_piece, std::unique_ptr<watchman_dir>> dirs;

  // If we think this dir was deleted, we'll avoid recursing
  // to its children when processing deletes.
  bool last_check_existed{true};

  watchman_dir(w_string name, watchman_dir* parent);

  watchman_dir* getChildDir(w_string_piece name) const;

  /**
   * Returns the direct child file named name, or nullptr if there is no such
   * entry.
   */
  watchman_file* getChildFile(w_string_piece name) const;

  /**
   * Walk up to the chain of dirs via ->parent to and then produce the full path
   * to this dir relative to the root of the watch.
   */
  w_string getFullPath() const;

  /**
   * Compute the full path to this dir and concatenate child with it, to produce
   * the path to the child.
   */
  w_string getFullPathToChild(w_string_piece child) const;
};
