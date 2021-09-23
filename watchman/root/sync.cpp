/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_root.h"

using namespace watchman;

void watchman_root::syncToNow(
    std::chrono::milliseconds timeout,
    std::vector<w_string>& cookieFileNames) {
  PerfSample sample("sync_to_now");
  auto root = shared_from_this();
  try {
    view()->syncToNow(root, timeout, cookieFileNames);
    if (sample.finish()) {
      root->addPerfSampleMetadata(sample);
      sample.add_meta(
          "sync_to_now",
          json_object(
              {{"success", json_boolean(true)},
               {"timeoutms", json_integer(timeout.count())}}));
      sample.log();
    }
  } catch (const std::exception& exc) {
    sample.force_log();
    sample.finish();
    root->addPerfSampleMetadata(sample);
    sample.add_meta(
        "sync_to_now",
        json_object(
            {{"success", json_boolean(false)},
             {"reason", w_string_to_json(exc.what())},
             {"timeoutms", json_integer(timeout.count())}}));
    sample.log();
    throw;
  }
}

/* vim:ts=2:sw=2:et:
 */
