/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/scm/Git.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace ::testing;
using namespace watchman;
using namespace std::literals::string_literals;

TEST(Git, add_then_remove_reports_as_change) {
  GitStatusAccumulator accumulator;

  accumulator.add(" A foo\0 A bar\0"s);
  accumulator.add(" D bar\0 D baz\0"s);

  auto result = accumulator.finalize();

  EXPECT_THAT(result.addedFiles, ElementsAre("foo"));
  EXPECT_THAT(result.changedFiles, ElementsAre("bar"));
  EXPECT_THAT(result.removedFiles, ElementsAre("baz"));
}
