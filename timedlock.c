/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef __APPLE__
// We get to emulate this function because Darwin doesn't implement it

int pthread_mutex_timedlock(pthread_mutex_t *m,
                            const struct timespec *deadline_ts) {
  int result;
  struct timeval now, deadline;
  int usec = 1;

  w_timespec_to_timeval(*deadline_ts, &deadline);

  while (true) {
    gettimeofday(&now, NULL);
    if (w_timeval_compare(now, deadline) >= 0) {
      return ETIMEDOUT;
    }

    result = pthread_mutex_trylock(m);
    if (result != EBUSY) {
      return result;
    }

    // Exponential backoff on the sleep period
    usec = MIN(usec * 2, 1024);
    if (now.tv_sec == deadline.tv_sec) {
      // Cap the sleep if we are close to the deadline
      usec = MIN(usec, deadline.tv_usec - now.tv_usec);
    }
    usleep(usec);
  }
}
#endif

/* vim:ts=2:sw=2:et:
 */
