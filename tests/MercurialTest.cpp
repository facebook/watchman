/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include "scm/Mercurial.h"
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>

using namespace std::chrono;

TEST(Mercurial, convertCommitDate) {
  auto date = watchman::Mercurial::convertCommitDate("1529420960.025200");
  auto result = duration_cast<seconds>(date.time_since_epoch()).count();
  auto expected = 1529420960;
  EXPECT_EQ(result, expected);
}
