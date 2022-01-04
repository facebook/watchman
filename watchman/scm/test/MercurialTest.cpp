/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/scm/Mercurial.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace ::testing;
using namespace watchman;
using namespace std::literals::string_literals;

TEST(Mercurial, add_then_remove_reports_as_change) {
  StatusAccumulator accumulator;

  accumulator.add("A foo\0A bar\0"s);
  accumulator.add("R bar\0R baz\0"s);

  auto result = accumulator.finalize();

  EXPECT_THAT(result.addedFiles, ElementsAre("foo"));
  EXPECT_THAT(result.changedFiles, ElementsAre("bar"));
  EXPECT_THAT(result.removedFiles, ElementsAre("baz"));
}
