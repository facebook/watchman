/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
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
