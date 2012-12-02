/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"
#include "thirdparty/tap.h"

int main(int argc, char **argv)
{
  char **dupd;
  json_t *args;
  (void)argc;
  (void)argv;

  plan_tests(8);

  args = json_array();
  json_array_append_new(args, json_string("one"));
  json_array_append_new(args, json_string("two"));
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

