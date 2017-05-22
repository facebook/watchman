/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_file {
  /* the parent dir */
  watchman_dir *parent;

  /* linkage to files ordered by changed time.
   * prev points to the address of `next` in the
   * previous file node, or the head of the list. */
  struct watchman_file **prev, *next;

  /* linkage to files ordered by common suffix.
   * suffix_prev points to the address of `suffix_next`
   * in the previous file node, or the head of the
   * suffix list. */
  struct watchman_file **suffix_prev, *suffix_next;

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
  watchman::FileInformation stat;

  /* the symbolic link target of this file.
   * Can be NULL if not a symlink, or we failed to read the target */
  w_string symlink_target;

  inline w_string_piece getName() const {
    auto lenPtr = (uint32_t*)(this + 1);
    auto data = (char*)(lenPtr + 1);
    return w_string_piece(data, *lenPtr);
  }

  void removeFromFileList();

  watchman_file() = delete;
  watchman_file(const watchman_file&) = delete;
  watchman_file& operator=(const watchman_file&) = delete;
  ~watchman_file();

  static std::unique_ptr<watchman_file, watchman_dir::Deleter> make(
      const w_string& name,
      watchman_dir* parent);

 private:
  void removeFromSuffixList();
};

void free_file_node(struct watchman_file* file);
