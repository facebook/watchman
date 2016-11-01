/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <algorithm>
#include "thirdparty/tap.h"

void w_log(int level, WATCHMAN_FMT_STRING(const char *fmt), ...) {
  unused_parameter(level);
  unused_parameter(fmt);
}

void test_suffix() {

  w_string_t *suffix = nullptr, *str = nullptr, *expect_suffix = nullptr;
  str = w_string_new_typed("", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  ok(!suffix, "empty string suffix");

  str = w_string_new_typed(".", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  ok(!suffix, "only one dot suffix");

  str = w_string_new_typed("endwithdot.", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  ok(!suffix, "end with dot");

  str = w_string_new_typed("nosuffix", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  ok(!suffix, "no suffix");

  str = w_string_new_typed(".beginwithdot", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  expect_suffix = w_string_new_typed("beginwithdot", W_STRING_UNICODE);
  ok(w_string_equal(suffix, expect_suffix), "begin with dot");

  str = w_string_new_typed("MainActivity.java", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  expect_suffix = w_string_new_typed("java", W_STRING_UNICODE);
  ok(w_string_equal(suffix, expect_suffix), "java suffix");

  // many '.' in name
  str = w_string_new_typed("index.android.bundle", W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  expect_suffix = w_string_new_typed("bundle", W_STRING_UNICODE);
  ok(w_string_equal(suffix, expect_suffix), "multi dots suffix");

  char too_long_name[129] = {0};
  for (char &ch : too_long_name) {ch = 'a'; }
  too_long_name[0] = '.';
  str = w_string_new_len_typed(too_long_name, sizeof(too_long_name), W_STRING_UNICODE);
  suffix = w_string_suffix(str);
  ok(!suffix, "too long suffix");

  if (str) w_string_delref(str);
  if (suffix) w_string_delref(suffix);
  if (expect_suffix) w_string_delref(expect_suffix);

}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  plan_tests(8);

  test_suffix();

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */
