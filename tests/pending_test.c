/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include "thirdparty/tap.h"
#include "thirdparty/libart/src/art.h"

struct pending_list {
  struct watchman_pending_fs *pending, *avail, *end;
};

struct watchman_pending_fs *next_pending(struct pending_list *list) {
  if (list->avail == list->end) {
    fail("make list alloc size bigger (used %u entries)",
         list->avail - list->pending);
    abort();
  }

  return list->avail++;
}

static void build_list(struct pending_list *list, w_string_t *parent_name,
                       size_t depth, size_t num_files, size_t num_dirs) {
  size_t i;
  for (i = 0; i < num_files; i++) {
    struct watchman_pending_fs *item = next_pending(list);
    item->path = w_string_make_printf("%.*s/file%d", parent_name->len,
                                      parent_name->buf, i);
    item->flags = W_PENDING_VIA_NOTIFY;
  }

  for (i = 0; i < num_dirs; i++) {
    struct watchman_pending_fs *item = next_pending(list);
    item->path = w_string_make_printf("%.*s/dir%d", parent_name->len,
                                      parent_name->buf, i);
    item->flags = W_PENDING_RECURSIVE;

    if (depth > 0) {
      build_list(list, item->path, depth - 1, num_files, num_dirs);
    }
  }
}

size_t process_items(struct watchman_pending_collection *coll) {
  struct watchman_pending_fs *item;
  size_t drained = 0;
  struct stat st;

  while ((item = w_pending_coll_pop(coll)) != NULL) {
    // To simulate looking at the file, we're just going to stat
    // ourselves over and over, as the path we put in the list
    // doesn't exist on the filesystem.  We're measuring hot cache
    // (best case) stat performance here.
    w_lstat(__FILE__, &st, true);
    w_pending_fs_free(item);

    drained++;
  }
  return drained;
}

// Simulate
static void bench_pending(void) {
  // These parameters give us 262140 items to track
  const size_t tree_depth = 7;
  const size_t num_files_per_dir = 8;
  const size_t num_dirs_per_dir = 4;
  w_string_t *root_name = w_string_new("/some/path");
  struct pending_list list;
  const size_t alloc_size = 280000;
  struct timeval start, end;

  list.pending = calloc(alloc_size, sizeof(struct watchman_pending_fs));
  list.avail = list.pending;
  list.end = list.pending + alloc_size;

  // Build a list ordered from the root (top) down to the leaves.
  build_list(&list, root_name, tree_depth, num_files_per_dir, num_dirs_per_dir);
  diag("built list with %u items", list.avail - list.pending);

  // Benchmark insertion in top-down order.
  {
    struct watchman_pending_collection coll;
    struct watchman_pending_fs *item;
    size_t drained = 0;

    w_pending_coll_init(&coll);

    gettimeofday(&start, NULL);
    for (item = list.pending; item < list.avail; item++) {
      w_pending_coll_add(&coll, item->path, item->now, item->flags);
    }
    drained = process_items(&coll);

    gettimeofday(&end, NULL);
    diag("took %.3fs to insert %u items into pending coll",
         w_timeval_diff(start, end), drained);
  }

  // and now in reverse order; this is from the leaves of the filesystem
  // tree up to the root, or bottom-up.  This simulates the workload of
  // a recursive delete of a filesystem tree.
  {
    struct watchman_pending_collection coll;
    struct watchman_pending_fs *item;
    size_t drained = 0;

    w_pending_coll_init(&coll);

    gettimeofday(&start, NULL);
    for (item = list.avail - 1; item >= list.pending; item--) {
      w_pending_coll_add(&coll, item->path, item->now, item->flags);
    }

    drained = process_items(&coll);

    gettimeofday(&end, NULL);
    diag("took %.3fs to reverse insert %u items into pending coll",
         w_timeval_diff(start, end), drained);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(1);
  bench_pending();
  pass("got here");

  return exit_status();
}
