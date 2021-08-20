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
#include <folly/portability/GTest.h>

using namespace std::chrono;

TEST(Mercurial, convertCommitDate) {
  auto date = watchman::Mercurial::convertCommitDate("1529420960.025200");
  auto result = duration_cast<seconds>(date.time_since_epoch()).count();
  auto expected = 1529420960;
  EXPECT_EQ(result, expected);
}
