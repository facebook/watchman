/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "MapUtil.h"
#include <string>
#include "watchman.h"
#include "watchman_string.h"
//#include <unordered_map>
#include "thirdparty/tap.h"

using watchman::mapContainsAny;

void test_map_contains_any() {
  std::unordered_map<w_string, int> uMap = {
      {"one", 1}, {"two", 2}, {"three", 3}};

  // Map contains key

  ok(mapContainsAny(uMap, "one"), "mapContainsAny single string present");

  ok(mapContainsAny(uMap, "one", "two"), "mapContainsAny two strings present");

  ok(mapContainsAny(uMap, "one", "two", "three"),
     "mapContainsAny three strings present");

  ok(mapContainsAny(uMap, "one", "xcase"),
     "mapContainsAny first string present");

  ok(mapContainsAny(uMap, "xcase", "two"),
     "mapContainsAny second string present");

  ok(mapContainsAny(uMap, "xcase1", "xcase2", "three"),
     "mapContainsAny last string present");

  // Map does not contain key

  ok(!mapContainsAny(uMap, "xcase"), "mapContainsAny single string absent");

  ok(!mapContainsAny(uMap, "xcase1", "xcase2"),
     "mapContainsAny two strings absent");

  ok(!mapContainsAny(uMap, "xcase1", "xcase2", "xcase3"),
     "mapContainsAny three strings absent");

  // Empty map tests
  std::unordered_map<w_string, w_string> eMap;

  ok(!mapContainsAny(eMap, "xcase1"), "mapContainsAny absent on empty mape");
}

int main(int, char**) {
  plan_tests(10);
  test_map_contains_any();

  return exit_status();
}
