/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/tap.h"

void w_request_shutdown(void) {}

bool w_should_log_to_clients(int level)
{
  unused_parameter(level);
  return true;
}

void w_log_to_clients(int level, const char *buf)
{
  unused_parameter(level);
  unused_parameter(buf);

  pass("made it into w_log_to_clients");
}

int main(int argc, char **argv)
{
  char huge[8192];
  (void)argc;
  (void)argv;

  plan_tests(2);

  memset(huge, 'X', sizeof(huge));
  huge[sizeof(huge)-1] = '\0';
  w_log(W_LOG_DBG, "test %s", huge);

  pass("made it to the end");

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */
