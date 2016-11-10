/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/attr.h>
#endif

bool did_file_change(struct watchman_stat *saved, struct watchman_stat *fresh) {
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

  if (!S_ISDIR(saved->mode)) {
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

void struct_stat_to_watchman_stat(const struct stat *st,
                                  struct watchman_stat *target) {
  target->size = (off_t)st->st_size;
  target->mode = st->st_mode;
  target->uid = st->st_uid;
  target->gid = st->st_gid;
  target->ino = st->st_ino;
  target->dev = st->st_dev;
  target->nlink = st->st_nlink;
  memcpy(&target->atime, &st->WATCHMAN_ST_TIMESPEC(a),
      sizeof(target->atime));
  memcpy(&target->mtime, &st->WATCHMAN_ST_TIMESPEC(m),
      sizeof(target->mtime));
  memcpy(&target->ctime, &st->WATCHMAN_ST_TIMESPEC(c),
      sizeof(target->ctime));
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
      1, sizeof(watchman_file) + w_string_embedded_size(name));
  std::unique_ptr<watchman_file, watchman_dir::Deleter> filePtr(
      file, watchman_dir::Deleter());

  auto targetName = file->getName();
  w_string_embedded_copy(targetName, name);
  w_string_addref(targetName);

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
