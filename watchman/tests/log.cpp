/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

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
