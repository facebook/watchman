/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <folly/portability/SysTime.h>

#ifdef __cplusplus
extern "C" {
#endif

void FILETIME_to_timespec(const FILETIME *ft, struct timespec *ts);
void FILETIME_LARGE_INTEGER_to_timespec(LARGE_INTEGER ft, struct timespec *ts);
void usleep(int64_t usec);
void sleep(int sec);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
