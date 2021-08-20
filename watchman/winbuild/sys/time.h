/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
