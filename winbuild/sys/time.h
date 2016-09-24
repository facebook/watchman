/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef SYS_TIME_H
#define SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

// Defined by winsock, but we're not pulling that in, so we get
// to define our own version of the struct.  We choose to define
// tv_sec as a time_t for the sake of sanity.  This is not ABI
// compatible with winsock, but we don't want or need that
struct timeval {
  time_t tv_sec;
  time_t tv_usec;
};

int gettimeofday(struct timeval *tv, void *ignored);
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
