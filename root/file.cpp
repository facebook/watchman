/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

bool did_file_change(
    const watchman::FileInformation* saved,
    const watchman::FileInformation* fresh) {
/* we have to compare this way because the stat structure
 * may contain fields that vary and that don't impact our
 * understanding of the file */

#define FIELD_CHG(name) \
  if (saved->name != fresh->name) { \
    return true; \
  }

  // Can't compare with memcmp due to padding and garbage in the struct
  // on OpenBSD, which has a 32-bit tv_sec + 64-bit tv_nsec
#define TIMESPEC_FIELD_CHG(wat) { \
  struct timespec a = saved->wat##time; \
  struct timespec b = fresh->wat##time; \
  if (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec) { \
    return true; \
  } \
}

  FIELD_CHG(mode);

  if (!saved->isDir()) {
    FIELD_CHG(size);
    FIELD_CHG(nlink);
  }
  FIELD_CHG(dev);
  FIELD_CHG(ino);
  FIELD_CHG(uid);
  FIELD_CHG(gid);
  // Don't care about st_blocks
  // Don't care about st_blksize
  // Don't care about st_atimespec
  TIMESPEC_FIELD_CHG(m);
  TIMESPEC_FIELD_CHG(c);

  return false;
}

void watchman_file::removeFromFileList() {
  if (next) {
    next->prev = prev;
  }
  // file->prev points to the address of either
  // `previous_file->next` OR `root->inner.latest_file`.
  // This next assignment is therefore fixing up either
  // the linkage from the prior file node or from the
  // head of the list.
  if (prev) {
    *prev = next;
  }
}

void watchman_file::removeFromSuffixList() {
  if (suffix_next) {
    suffix_next->suffix_prev = suffix_prev;
  }
  // suffix_prev points to the address of either
  // `previous_file->suffix_next` OR the `file_list_head.head`
  // tracked in `root->inner.suffixes`.
  // This next assignment is therefore fixing up either
  // the linkage from the prior file node or from the
  // head of the list.
  if (suffix_prev) {
    *suffix_prev = suffix_next;
  }
}

/* We embed our name string in the tail end of the struct that we're
 * allocating here.  This turns out to be more memory efficient due
 * to the way that the allocator bins sizeof(watchman_file); there's
 * a bit of unusable space after the end of the structure that happens
 * to be about the right size to fit a typical filename.
 * Embedding the name in the end allows us to make the most of this
 * memory and free up the separate heap allocation for file_name.
 */
std::unique_ptr<watchman_file, watchman_dir::Deleter> watchman_file::make(
    const w_string& name,
    watchman_dir* parent) {
  auto file = (watchman_file*)calloc(
      1, sizeof(watchman_file) + sizeof(uint32_t) + name.size() + 1);
  std::unique_ptr<watchman_file, watchman_dir::Deleter> filePtr(
      file, watchman_dir::Deleter());

  auto lenPtr = (uint32_t*)(file + 1);
  *lenPtr = name.size();

  auto data = (char*)(lenPtr + 1);
  memcpy(data, name.data(), name.size());
  data[name.size()] = 0;

  file->parent = parent;
  file->exists = true;

  return filePtr;
}

watchman_file::~watchman_file() {
  removeFromFileList();
  removeFromSuffixList();
}

void free_file_node(struct watchman_file* file) {
  file->~watchman_file();
  free(file);
}

/* vim:ts=2:sw=2:et:
 */
