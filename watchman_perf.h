/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_PERF_H
#define WATCHMAN_PERF_H
#include "thirdparty/jansson/jansson.h"
struct watchman_root;
typedef struct watchman_root w_root_t;

// Performance metrics sampling

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_perf_sample {
  // What we're sampling across
  const char *description;

  // Additional arbitrary information.  This is either NULL
  // or is a json object with various properties set inside it
  json_ref meta_data;

  // Measure the wall time
  struct timeval time_begin, time_end, duration;

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
  struct rusage usage_begin, usage_end, usage;
#endif

  // Initialize and mark the start of a sample
  watchman_perf_sample(const char* description);
  watchman_perf_sample(const watchman_perf_sample&) = delete;
  watchman_perf_sample(watchman_perf_sample&&) = delete;

  // Augment any configuration policy and cause this sample to be logged if the
  // walltime exceeds the specified number of seconds (fractions are supported)
  void set_wall_time_thresh(double thresh);

  // Mark the end of a sample.  Returns true if the policy is to log this
  // sample.  This allows the caller to conditionally build and add metadata
  bool finish();

  // Annotate the sample with metadata
  void add_meta(const char* key, json_ref&& val);

  // Annotate the sample with some standard metadata taken from a root.
  void add_root_meta(const std::shared_ptr<w_root_t>& root);

  // Force the sample to go to the log
  void force_log();

  // If will_log is set, arranges to send the sample to the log
  void log();
};
typedef struct watchman_perf_sample w_perf_t;



#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
