/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <memory>
#include <unordered_map>
#include "watchman_string.h"

struct watchman_file;
struct watchman_dir;

namespace watchman {

/** Keeps track of the state of the filesystem in-memory. */
struct InMemoryView {
  std::unique_ptr<watchman_dir> root_dir;
  w_string root_path;

  /* the most recently changed file */
  struct watchman_file* latest_file{0};

  /* Holds the list head for files of a given suffix */
  struct file_list_head {
    watchman_file* head{nullptr};
  };

  /* Holds the list heads for all known suffixes */
  std::unordered_map<w_string, std::unique_ptr<file_list_head>> suffixes;

  explicit InMemoryView(const w_string& root_path);

  /** Updates the otime for the file and bubbles it to the front of recency
   * index */
  void markFileChanged(
      watchman_file* file,
      const struct timeval& now,
      uint32_t tick);

  watchman_dir* resolveDir(const w_string& dirname, bool create);
  const watchman_dir* resolveDir(const w_string& dirname) const;
};
}
