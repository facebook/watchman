/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "scm/Mercurial.h"
#include "thirdparty/tap.h"
#include "watchman.h"

using namespace std::chrono;

void test_convert_date() {
  auto date = watchman::Mercurial::convertCommitDate("1529420960.025200");
  auto result = duration_cast<seconds>(date.time_since_epoch()).count();
  auto expected = 1529420960;
  ok(result == expected, "Expected %d but observed %d", expected, result);
}

int main(int, char**) {
  plan_tests(1);
  test_convert_date();

  return exit_status();
}
