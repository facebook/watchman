/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "MapUtil.h"
#include <string>
#include "watchman.h"
#include "watchman_string.h"
//#include <unordered_map>
#include "thirdparty/tap.h"

using watchman::mapContainsAny;
using watchman::mapContainsAnyOf;

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

void test_map_contains_any_of() {
  std::unordered_map<w_string, int> uMap = {
      {"one", 1}, {"two", 2}, {"three", 3}};

  {
    // Using iterator to do the lookup
    std::unordered_set<w_string> kSet = {"one"};
    ok(mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf single string present");

    kSet.emplace("two");
    ok(mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf two strings present");

    kSet.emplace("three");
    ok(mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf all strings present");
  }
  {
    std::unordered_set<w_string> kSet = {"one", "xcase1", "xcase2", "xcase3"};
    ok(mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf one of several strings present");

    kSet.emplace("two");
    ok(mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf two of several strings present");
  }
  {
    std::unordered_set<w_string> kSet;
    ok(!mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf empty set");

    kSet.emplace("xcase1");
    ok(!mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf single string absent");

    kSet.emplace("xcase2");
    ok(!mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf two strings absent");

    kSet.emplace("xcase3");
    ok(!mapContainsAnyOf(uMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf three strings absent");
  }

  // Empty map tests
  {
    std::unordered_map<w_string, w_string> eMap;

    std::unordered_set<w_string> kSet;
    ok(!mapContainsAnyOf(eMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf absent on empty map and set");

    kSet.emplace("one");
    ok(!mapContainsAnyOf(eMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf absent on empty map and non-empty set");

    kSet.emplace("two");
    ok(!mapContainsAnyOf(eMap, kSet.begin(), kSet.end()),
       "mapContainsAnyOf absent on empty map and 2 item set");
  }
}

int main(int, char**) {
  plan_tests(22);
  test_map_contains_any();
  test_map_contains_any_of();

  return exit_status();
}
