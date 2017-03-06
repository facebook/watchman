/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void watchman_root::considerAgeOut() {
  time_t now;

  if (gc_interval == 0) {
    return;
  }

  time(&now);

  if (now <= view()->getLastAgeOutTimeStamp() + gc_interval) {
    // Don't check too often
    return;
  }

  performAgeOut(std::chrono::seconds(gc_age));
}

void watchman_root::performAgeOut(std::chrono::seconds min_age) {
  // Find deleted nodes older than the gc_age setting.
  // This is particularly useful in cases where your tree observes a
  // large number of creates and deletes for many unique filenames in
  // a given dir (eg: temporary/randomized filenames generated as part
  // of build tooling or atomic renames)
  w_perf_t sample("age_out");

  view()->ageOut(sample, std::chrono::seconds(min_age));

  // Age out cursors too.
  {
    auto cursors = inner.cursors.wlock();
    auto it = cursors->begin();
    while (it != cursors->end()) {
      if (it->second < view()->getLastAgeOutTickValue()) {
        it = cursors->erase(it);
      } else {
        ++it;
      }
    }
  }
  if (sample.finish()) {
    sample.add_root_meta(shared_from_this());
    sample.log();
  }
}

/* vim:ts=2:sw=2:et:
 */
