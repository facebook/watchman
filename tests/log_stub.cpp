/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"

// These are logging stubs to facilitate testing code that pulls in w_log
// either directly or indirectly.

void w_request_shutdown(void) {}

bool w_should_log_to_clients(int level)
{
  unused_parameter(level);
  return false;
}

void w_log_to_clients(int level, const char *buf)
{
  unused_parameter(level);
  unused_parameter(buf);
}
