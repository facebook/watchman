/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// 100's of nanoseconds since the FILETIME epoch
static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

// FILETIME is expressed in 100's of nanoseconds
void FILETIME_LARGE_INTEGER_to_timespec(LARGE_INTEGER ft, struct timespec* ts) {
  static const uint32_t factor = WATCHMAN_NSEC_IN_SEC / 100;

  ft.QuadPart -= EPOCH;
  ts->tv_sec = ft.QuadPart / factor;
  ft.QuadPart -= ts->tv_sec * factor;
  ts->tv_nsec = ft.QuadPart * 100;
}

void FILETIME_to_timespec(const FILETIME* ft, struct timespec* ts) {
  LARGE_INTEGER li;

  li.HighPart = ft->dwHighDateTime;
  li.LowPart = ft->dwLowDateTime;

  FILETIME_LARGE_INTEGER_to_timespec(li, ts);
}

static void timespec_to_timeval(const struct timespec* ts, struct timeval* tv) {
  tv->tv_sec = (long)ts->tv_sec;
  tv->tv_usec = (long)(ts->tv_nsec / WATCHMAN_NSEC_IN_USEC);
}
