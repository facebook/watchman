/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "thirdparty/tap.h"

int main(int argc, char **argv)
{
  char **dupd;
  (void)argc;
  (void)argv;

  plan_tests(8);

  auto args = json_array();
  json_array_append_new(args, typed_string_to_json("one", W_STRING_UNICODE));
  json_array_append_new(args, typed_string_to_json("two", W_STRING_UNICODE));
  ok(json_array_size(args) == 2, "sanity check array size");

  dupd = w_argv_copy_from_json(args, 0);
  ok(dupd != NULL, "got a result");
  ok(dupd[0] != NULL && !strcmp(dupd[0], "one"), "got one");
  ok(dupd[1] != NULL && !strcmp(dupd[1], "two"), "got two");
  ok(dupd[2] == NULL, "terminated");
  free(dupd);

  dupd = w_argv_copy_from_json(args, 1);
  ok(dupd != NULL, "got a result");
  ok(dupd[0] != NULL && !strcmp(dupd[0], "two"), "got two");
  ok(dupd[1] == NULL, "terminated");
  free(dupd);

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */
