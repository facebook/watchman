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

#pragma once

// Helpers for pasting __LINE__ for symbol generation
#define w_paste2(pre, post) pre##post
#define w_paste1(pre, post) w_paste2(pre, post)
#define w_gen_symbol(pre) w_paste1(pre, __LINE__)

#ifndef WATCHMAN_FMT_STRING
#define WATCHMAN_FMT_STRING(x) x
#endif

#ifdef __GNUC__
#define WATCHMAN_FMT_ATTR(fmt_param_no, dots_param_no) \
  __attribute__((__format__(__printf__, fmt_param_no, dots_param_no)))
#endif

#ifndef WATCHMAN_FMT_ATTR
#define WATCHMAN_FMT_ATTR(fmt_param_no, dots_param_no) /* nothing */
#endif
