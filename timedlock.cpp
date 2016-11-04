/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#ifdef __APPLE__
// We get to emulate these functions because Darwin doesn't implement them

#define timed_helper(func_name, object_type, try_func)                   \
  int func_name(object_type* lock, const struct timespec* deadline_ts) { \
    int result;                                                          \
    struct timeval now, deadline;                                        \
    int usec = 1;                                                        \
    w_timespec_to_timeval(*deadline_ts, &deadline);                      \
    while (true) {                                                       \
      gettimeofday(&now, nullptr);                                       \
      if (w_timeval_compare(now, deadline) >= 0) {                       \
        return ETIMEDOUT;                                                \
      }                                                                  \
      result = try_func(lock);                                           \
      if (result != EBUSY) {                                             \
        return result;                                                   \
      }                                                                  \
      /* Exponential backoff on the sleep period */                      \
      usec = std::min(usec * 2, 1024);                                   \
      if (now.tv_sec == deadline.tv_sec) {                               \
        /* Cap the sleep if we are close to the deadline */              \
        usec = std::min(usec, deadline.tv_usec - now.tv_usec);           \
      }                                                                  \
      /* sleep override */ usleep(usec);                                 \
    }                                                                    \
  }

timed_helper(pthread_mutex_timedlock, pthread_mutex_t, pthread_mutex_trylock)
timed_helper(pthread_rwlock_timedwrlock, pthread_rwlock_t,
             pthread_rwlock_trywrlock)
timed_helper(pthread_rwlock_timedrdlock, pthread_rwlock_t,
             pthread_rwlock_tryrdlock)

#endif

/* vim:ts=2:sw=2:et:
 */
