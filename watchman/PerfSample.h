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

#pragma once

#include <string>
#include <vector>
#include "watchman/thirdparty/jansson/jansson.h"

// Performance metrics sampling

namespace watchman {

class PerfSample {
 public:
  // What we're sampling across
  const char* description;

  // Additional arbitrary information.
  // This is a json object with various properties set inside it
  json_ref meta_data{json_object()};

  // Measure the wall time
  timeval time_begin;
  timeval time_end;
  timeval duration;

  // If set to true, the sample should be sent to the logging
  // mechanism
  bool will_log{false};

  // If non-zero, force logging on if the wall time is greater
  // that this value
  double wall_time_elapsed_thresh{0};

#ifdef HAVE_SYS_RESOURCE_H
  // When available (posix), record these process-wide stats.
  // It can be difficult to attribute these directly to the
  // action being sampled because there can be multiple
  // watched roots and these metrics include the usage from
  // all of them.
  struct rusage usage_begin;
  struct rusage usage_end;
  struct rusage usage;
#endif

  // Initialize and mark the start of a sample
  explicit PerfSample(const char* description);

  PerfSample(const PerfSample&) = delete;
  PerfSample(PerfSample&&) = delete;
  PerfSample& operator=(const PerfSample&) = delete;
  PerfSample& operator=(PerfSample&&) = delete;

  // Augment any configuration policy and cause this sample to be logged if the
  // walltime exceeds the specified number of seconds (fractions are supported)
  void set_wall_time_thresh(double thresh);

  // Mark the end of a sample.  Returns true if the policy is to log this
  // sample.  This allows the caller to conditionally build and add metadata
  bool finish();

  // Annotate the sample with metadata
  void add_meta(const char* key, json_ref&& val);

  // Force the sample to go to the log
  void force_log();

  // If will_log is set, arranges to send the sample to the log
  void log();
};

void perf_shutdown();

void processSamples(
    size_t argv_limit,
    size_t maximum_batch_size,
    json_ref samples,
    std::function<void(std::vector<std::string>)> command_line,
    std::function<void(std::string)> single_large_sample);

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
