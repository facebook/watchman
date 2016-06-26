/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// We do our own abort because the standard implementation invokes
// troubleshooting UI that the user won't care about, and because
// this actually doesn't work properly in our test harness
void w_abort(void) {
  ignore_result(
      write(STDERR_FILENO, "aborting\n", (unsigned int)strlen("aborting\n")));
  exit(1);
}
