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
#include "watchman/Logging.h"

using namespace watchman;

void w_request_shutdown(void) {}

TEST(Log, logging) {
  char huge[8192];
  bool logged = false;

  auto sub = watchman::getLog().subscribe(
      watchman::DBG, [&logged]() { logged = true; });

  memset(huge, 'X', sizeof(huge));
  huge[sizeof(huge) - 1] = '\0';

  logf(DBG, "test {}", huge);

  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;
  sub->getPending(pending);
  EXPECT_FALSE(pending.empty()) << "got an item from our subscription";
  EXPECT_TRUE(logged);
}

/* vim:ts=2:sw=2:et:
 */
