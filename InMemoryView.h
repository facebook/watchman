/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
namespace watchman {

/** Keeps track of the state of the filesystem in-memory. */
struct InMemoryView {
  std::unique_ptr<watchman_dir> root_dir;

  /* the most recently changed file */
  struct watchman_file* latest_file{0};

  /* Holds the list head for files of a given suffix */
  struct file_list_head {
    watchman_file* head{nullptr};
  };

  /* Holds the list heads for all known suffixes */
  std::unordered_map<w_string, std::unique_ptr<file_list_head>> suffixes;
};
}
