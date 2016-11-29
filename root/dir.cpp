/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void watchman_dir::Deleter::operator()(watchman_file* file) const {
  free_file_node(file);
}

watchman_dir::watchman_dir(w_string name, watchman_dir* parent)
    : name(name), parent(parent) {}

w_string watchman_dir::getFullPath() const {
  return w_dir_path_cat_str(this, w_string_piece());
}

watchman_file* watchman_dir::getChildFile(w_string name) const {
  auto it = files.find(name.piece());
  if (it == files.end()) {
    return nullptr;
  }
  return it->second.get();
}

watchman_dir* watchman_dir::getChildDir(w_string name) const {
  auto it = dirs.find(name);
  if (it == dirs.end()) {
    return nullptr;
  }
  return it->second.get();
}

/* vim:ts=2:sw=2:et:
 */
