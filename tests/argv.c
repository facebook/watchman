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
  bool res;
  int num;
  char **args;

  plan_tests(4);

  ok(w_argv_parse("one two", &num, &args), "parse 'one two'");
  ok(num == 2, "got 2 elements");
  ok(!strcmp(args[0], "one"), "got 'one'");
  ok(!strcmp(args[1], "two"), "got 'two'");

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

