/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

#include "InMemoryView.h"

namespace watchman {

static void insert_at_head_of_file_list(
    InMemoryView* view,
    struct watchman_file* file) {
  file->next = view->latest_file;
  if (file->next) {
    file->next->prev = &file->next;
  }
  view->latest_file = file;
  file->prev = &view->latest_file;
}

void InMemoryView::markFileChanged(
    watchman_file* file,
    const struct timeval& now,
    uint32_t tick) {
  file->otime.timestamp = now.tv_sec;
  file->otime.ticks = tick;

  if (latest_file != file) {
    // unlink from list
    remove_from_file_list(file);

    // and move to the head
    insert_at_head_of_file_list(this, file);
  }
}
}
