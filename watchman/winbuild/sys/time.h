/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef SYS_TIME_H
#define SYS_TIME_H

#include <folly/portability/SysTime.h>

#ifdef __cplusplus
extern "C" {
#endif

void FILETIME_to_timespec(const FILETIME* ft, struct timespec* ts);
void FILETIME_LARGE_INTEGER_to_timespec(LARGE_INTEGER ft, struct timespec* ts);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
