/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void consider_age_out(struct write_locked_watchman_root *lock)
{
  time_t now;

  if (lock->root->gc_interval == 0) {
    return;
  }

  time(&now);

  if (now <= lock->root->inner.view.getLastAgeOutTimeStamp() +
          lock->root->gc_interval) {
    // Don't check too often
    return;
  }

  w_root_perform_age_out(lock, lock->root->gc_age);
}

// Find deleted nodes older than the gc_age setting.
// This is particularly useful in cases where your tree observes a
// large number of creates and deletes for many unique filenames in
// a given dir (eg: temporary/randomized filenames generated as part
// of build tooling or atomic renames)
void w_root_perform_age_out(struct write_locked_watchman_root *lock,
                            int min_age) {
  w_perf_t sample("age_out");

  lock->root->inner.view.ageOut(sample, std::chrono::seconds(min_age));

  // Age out cursors too.
  {
    auto cursors = lock->root->inner.cursors.wlock();
    auto it = cursors->begin();
    while (it != cursors->end()) {
      if (it->second < lock->root->inner.view.getLastAgeOutTickValue()) {
        it = cursors->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (sample.finish()) {
    sample.add_root_meta(lock->root);
    sample.log();
  }
}

/* vim:ts=2:sw=2:et:
 */
