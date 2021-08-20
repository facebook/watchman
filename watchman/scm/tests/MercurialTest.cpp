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

#include "watchman/scm/Mercurial.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace ::testing;
using namespace watchman;
using namespace std::literals::string_literals;

TEST(Mercurial, add_then_remove_reports_as_change) {
  StatusAccumulator accumulator;

  accumulator.add("A foo\0A bar\0"s);
  accumulator.add("D bar\0"s);

  auto result = accumulator.finalize();

  EXPECT_THAT(result.addedFiles, ElementsAre("foo"));
  EXPECT_THAT(result.changedFiles, ElementsAre("bar"));
  EXPECT_THAT(result.removedFiles, ElementsAre());
}
