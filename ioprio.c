/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __linux__
#include <syscall.h>

/* There's no easily included header for this, so we recreate these here */
enum {
  IOPRIO_CLASS_NONE,
  IOPRIO_CLASS_RT,
  IOPRIO_CLASS_BE,
  IOPRIO_CLASS_IDLE,
};
enum {
  IOPRIO_WHO_PROCESS = 1,
  IOPRIO_WHO_PGRP,
  IOPRIO_WHO_USER,
};
#define IOPRIO_CLASS_SHIFT      (13)
#define IOPRIO_PRIO_MASK        ((1UL << IOPRIO_CLASS_SHIFT) - 1)
#define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | data)

#endif
#ifdef __APPLE__
#include <sys/resource.h>
#endif

static void adjust_ioprio(bool low) {
#if defined(__APPLE__) && defined(IOPOL_STANDARD)
  setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD,
      low ? IOPOL_THROTTLE : IOPOL_STANDARD);
#endif
#ifdef __linux__
  syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
      low ? IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)
          : IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4));
#endif
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(),
      low ? THREAD_MODE_BACKGROUND_BEGIN
          : THREAD_MODE_BACKGROUND_END);
#endif
  unused_parameter(low);
}

void w_ioprio_set_low(void) {
  adjust_ioprio(true);
}

void w_ioprio_set_normal(void) {
  adjust_ioprio(false);
}

/* vim:ts=2:sw=2:et:
 */
