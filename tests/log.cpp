/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "Logging.h"
#include "thirdparty/tap.h"

void w_request_shutdown(void) {}

int main(int, char**) {
  char huge[8192];

  plan_tests(3);
  auto sub = watchman::getLog().subscribe(
      watchman::DBG, []() { pass("made it to logging callback"); });

  memset(huge, 'X', sizeof(huge));
  huge[sizeof(huge)-1] = '\0';

  w_log(W_LOG_DBG, "test %s", huge);

  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;
  sub->getPending(pending);
  ok(!pending.empty(), "got an item from our subscription");

  pass("made it to the end");

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */
