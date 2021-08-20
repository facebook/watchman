/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman_root.h"

void watchman_root::considerAgeOut() {
  if (gc_interval.count() == 0) {
    return;
  }

  auto now = std::chrono::system_clock::now();
  if (now <= view()->getLastAgeOutTimeStamp() + gc_interval) {
    // Don't check too often
    return;
  }

  performAgeOut(gc_age);
}

void watchman_root::performAgeOut(std::chrono::seconds min_age) {
  // Find deleted nodes older than the gc_age setting.
  // This is particularly useful in cases where your tree observes a
  // large number of creates and deletes for many unique filenames in
  // a given dir (eg: temporary/randomized filenames generated as part
  // of build tooling or atomic renames)
  watchman::PerfSample sample("age_out");

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
    addPerfSampleMetadata(sample);
    sample.log();
  }
}

/* vim:ts=2:sw=2:et:
 */
