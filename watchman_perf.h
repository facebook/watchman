/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_PERF_H
#define WATCHMAN_PERF_H

// Performance metrics sampling

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_perf_sample {
  // The path that we're operating on
  w_string_t *root_path;

  // What we're sampling across
  const char *description;

  // Additional arbitrary information.  This is either NULL
  // or is a json object with various properties set inside it
  json_t *meta_data;

  // Measure the wall time
  struct timeval time_begin, time_end, duration;

  // If set to true, the sample should be sent to the logging
  // mechanism
  bool will_log;

  // If non-zero, force logging on if the wall time is greater
  // that this value
  double wall_time_elapsed_thresh;

#ifdef HAVE_SYS_RESOURCE_H
  // When available (posix), record these process-wide stats.
  // It can be difficult to attribute these directly to the
  // action being sampled because there can be multiple
  // watched roots and these metrics include the usage from
  // all of them.
  struct rusage usage_begin, usage_end, usage;
#endif
};
typedef struct watchman_perf_sample w_perf_t;

// Initialize and mark the start of a sample
void w_perf_start(w_perf_t *perf, const char *description);

// Release any resources associated with the sample
void w_perf_destroy(w_perf_t *perf);

// Augment any configuration policy and cause this sample to be logged if the
// walltime exceeds the specified number of seconds (fractions are supported)
void w_perf_set_wall_time_thresh(w_perf_t *perf, double thresh);

// Mark the end of a sample.  Returns true if the policy is to log this
// sample.  This allows the caller to conditionally build and add metadata
bool w_perf_finish(w_perf_t *perf);

// Annotate the sample with metadata
void w_perf_add_meta(w_perf_t *perf, const char *key, json_t *val);

// Annotate the sample with some standard metadata taken from a root.
void w_perf_add_root_meta(w_perf_t *perf, w_root_t *root);

// Force the sample to go to the log
void w_perf_force_log(w_perf_t *perf);

// If will_log is set, arranges to send the sample to the log
void w_perf_log(w_perf_t *perf);



#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
