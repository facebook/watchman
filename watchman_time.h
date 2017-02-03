/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"
#include <chrono>

/* Return a timespec holding the equivalent of the supplied duration */
template <class Rep, class Period>
inline timespec
durationToTimeSpecDuration(const std::chrono::duration<Rep, Period> &d) {
  timespec ts{0,0};

  ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(d).count();
  ts.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   d - std::chrono::seconds(ts.tv_sec))
                   .count();
  return ts;
}

/* Return a timespec holding an absolute time equivalent to the provided
 * system_clock timepoint */
inline timespec
systemClockToTimeSpec(const std::chrono::system_clock::time_point &p) {
  /* It just so happens that the epoch for system_clock is the same as the
   * epoch for timespec, so we can use our duration helper on the duration
   * since the epoch. */
  return durationToTimeSpecDuration(p.time_since_epoch());
}

void w_timeoutms_to_abs_timespec(int timeoutms, struct timespec *deadline);

/* compare two timevals and return -1 if a is < b, 0 if a == b,
 * or 1 if b > a */
static inline int w_timeval_compare(struct timeval a, struct timeval b)
{
  if (a.tv_sec < b.tv_sec) {
    return -1;
  }
  if (a.tv_sec > b.tv_sec) {
    return 1;
  }
  if (a.tv_usec < b.tv_usec) {
    return -1;
  }
  if (a.tv_usec > b.tv_usec) {
    return 1;
  }
  return 0;
}

#define WATCHMAN_USEC_IN_SEC 1000000
#define WATCHMAN_NSEC_IN_USEC 1000
#define WATCHMAN_NSEC_IN_SEC (1000 * 1000 * 1000)
#define WATCHMAN_NSEC_IN_MSEC 1000000

#if defined(__APPLE__) || defined(__FreeBSD__) \
 || (defined(__NetBSD__) && (__NetBSD_Version__ < 6099000000))
/* BSD-style subsecond timespec */
#define WATCHMAN_ST_TIMESPEC(type) st_##type##timespec
#else
/* POSIX standard timespec */
#define WATCHMAN_ST_TIMESPEC(type) st_##type##tim
#endif

static inline void w_timeval_add(const struct timeval a,
    const struct timeval b, struct timeval *result)
{
  result->tv_sec = a.tv_sec + b.tv_sec;
  result->tv_usec = a.tv_usec + b.tv_usec;

  if (result->tv_usec > WATCHMAN_USEC_IN_SEC) {
    result->tv_sec++;
    result->tv_usec -= WATCHMAN_USEC_IN_SEC;
  }
}

static inline void w_timeval_sub(const struct timeval a,
    const struct timeval b, struct timeval *result)
{
  result->tv_sec = a.tv_sec - b.tv_sec;
  result->tv_usec = a.tv_usec - b.tv_usec;

  if (result->tv_usec < 0) {
    result->tv_sec--;
    result->tv_usec += WATCHMAN_USEC_IN_SEC;
  }
}

static inline void w_timeval_to_timespec(
    const struct timeval a, struct timespec *ts)
{
  ts->tv_sec = a.tv_sec;
  ts->tv_nsec = a.tv_usec * WATCHMAN_NSEC_IN_USEC;
}

static inline void w_timespec_to_timeval(
    const struct timespec ts, struct timeval *tv) {
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / WATCHMAN_NSEC_IN_USEC;
}

// Convert a timeval to a double that holds the fractional number of seconds
static inline double w_timeval_abs_seconds(struct timeval tv){
  double val = (double)tv.tv_sec;
  val += ((double)tv.tv_usec)/WATCHMAN_USEC_IN_SEC;
  return val;
}

static inline double w_timeval_diff(struct timeval start, struct timeval end)
{
  double s = start.tv_sec + ((double)start.tv_usec)/WATCHMAN_USEC_IN_SEC;
  double e = end.tv_sec + ((double)end.tv_usec)/WATCHMAN_USEC_IN_SEC;

  return e - s;
}
