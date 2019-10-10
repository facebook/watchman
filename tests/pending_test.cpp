/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>

static void build_list(
    std::vector<watchman_pending_fs>* list,
    struct timeval* now,
    const w_string& parent_name,
    size_t depth,
    size_t num_files,
    size_t num_dirs) {
  size_t i;
  for (i = 0; i < num_files; i++) {
    list->emplace_back(
        w_string::build(parent_name, "/file", i), *now, W_PENDING_VIA_NOTIFY);
  }

  for (i = 0; i < num_dirs; i++) {
    list->emplace_back(
        w_string::build(parent_name, "/dir", i), *now, W_PENDING_RECURSIVE);

    if (depth > 0) {
      build_list(list, now, list->back().path, depth - 1, num_files, num_dirs);
    }
  }
}

size_t process_items(PendingCollection::LockedPtr& coll) {
  size_t drained = 0;

  auto item = coll->stealItems();
  while (item) {
    drained++;
    item = std::move(item->next);
  }
  return drained;
}

// Simulate
TEST(Pending, bench) {
  // These parameters give us 262140 items to track
  const size_t tree_depth = 7;
  const size_t num_files_per_dir = 8;
  const size_t num_dirs_per_dir = 4;
  w_string root_name("/some/path", W_STRING_BYTE);
  std::vector<watchman_pending_fs> list;
  const size_t alloc_size = 280000;
  struct timeval start, end;

  list.reserve(alloc_size);

  // Build a list ordered from the root (top) down to the leaves.
  gettimeofday(&start, nullptr);
  build_list(
      &list,
      &start,
      root_name,
      tree_depth,
      num_files_per_dir,
      num_dirs_per_dir);
  XLOG(ERR) << "built list with " << list.size() << " items";

  // Benchmark insertion in top-down order.
  {
    PendingCollection coll;
    size_t drained = 0;
    auto lock = coll.lock();

    gettimeofday(&start, NULL);
    for (auto& item : list) {
      lock->add(item.path, item.now, item.flags);
    }
    drained = process_items(lock);

    gettimeofday(&end, NULL);
    XLOG(ERR) << "took " << w_timeval_diff(start, end) << "s to insert "
              << drained << " items into pending coll";
  }

  // and now in reverse order; this is from the leaves of the filesystem
  // tree up to the root, or bottom-up.  This simulates the workload of
  // a recursive delete of a filesystem tree.
  {
    PendingCollection coll;
    size_t drained = 0;
    auto lock = coll.lock();

    gettimeofday(&start, NULL);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
      auto& item = *it;
      lock->add(item.path, item.now, item.flags);
    }

    drained = process_items(lock);

    gettimeofday(&end, NULL);
    XLOG(ERR) << "took " << w_timeval_diff(start, end) << "s to reverse insert "
              << drained << " items into pending coll";
  }
}
