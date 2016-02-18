/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

void w_timeoutms_to_abs_timespec(int timeoutms, struct timespec *deadline) {
  struct timeval now, delta, target;

  /* compute deadline */
  gettimeofday(&now, NULL);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, deadline);
}

/* vim:ts=2:sw=2:et:
 */
