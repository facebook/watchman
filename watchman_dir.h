/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <unordered_map>

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

  /* child dirs contained in this dir (keyed by dir->path) */
  std::unordered_map<w_string, std::unique_ptr<watchman_dir>> dirs;

  // If we think this dir was deleted, we'll avoid recursing
  // to its children when processing deletes
  bool last_check_existed{true};

  watchman_dir(w_string name, watchman_dir* parent);
  watchman_dir* getChildDir(w_string name) const;

  /** Returns the direct child file named name, or nullptr
   * if there is no such entry */
  watchman_file* getChildFile(w_string name) const;
  w_string getFullPath() const;
};
void delete_dir(struct watchman_dir* dir);
