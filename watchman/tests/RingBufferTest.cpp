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

#include <folly/portability/GTest.h>

#include "watchman/RingBuffer.h"

using namespace watchman;

TEST(RingBufferTest, writes_can_be_read) {
  RingBuffer<int> rb{2};
  rb.write(10);
  rb.write(11);
  auto result = rb.readAll();
  EXPECT_EQ(2, result.size());
  EXPECT_EQ(10, result[0]);
  EXPECT_EQ(11, result[1]);

  rb.write(12);
  result = rb.readAll();
  EXPECT_EQ(11, result[0]);
  EXPECT_EQ(12, result[1]);
}

TEST(RingBufferTest, writes_can_be_cleared) {
  RingBuffer<int> rb{10};
  rb.write(3);
  rb.write(4);
  auto result = rb.readAll();
  EXPECT_EQ(2, result.size());
  EXPECT_EQ(3, result[0]);
  EXPECT_EQ(4, result[1]);
  rb.clear();
  rb.write(5);
  result = rb.readAll();
  EXPECT_EQ(1, result.size());
  EXPECT_EQ(5, result[0]);
}
