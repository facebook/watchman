/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/OptionSet.h"
#include <folly/portability/GTest.h>

using namespace watchman;

namespace {

struct ColorSet : OptionSet<ColorSet, uint8_t> {
  using OptionSet::OptionSet;
  static const NameTable table;
};

constexpr inline auto CM_RED = ColorSet::raw(1);
constexpr inline auto CM_GREEN = ColorSet::raw(2);
constexpr inline auto CM_BLUE = ColorSet::raw(4);

const ColorSet::NameTable ColorSet::table = {
    {CM_RED, "RED"},
    {CM_GREEN, "GREEN"},
    {CM_BLUE, "BLUE"},
};

} // namespace

TEST(OptionSet, initialization_from_zero) {
  ColorSet set = 0;
  set = 0;

  // Does not compile:
  // set = 1;

  // Does not compile:
  // int i = 0;
  // set = i;
}

TEST(OptionSet, default_is_empty) {
  ColorSet set;
  EXPECT_FALSE(set);
}

TEST(OptionSet, assignment_operators) {
  ColorSet set;
  set |= CM_RED;
  EXPECT_EQ(CM_RED, set);
  set &= CM_GREEN;
  EXPECT_EQ(ColorSet{}, set);
}

TEST(OptionSet, format) {
  EXPECT_EQ("", ColorSet{}.format());
  EXPECT_EQ("RED", CM_RED.format());
  EXPECT_EQ("GREEN", CM_GREEN.format());
  EXPECT_EQ("BLUE", CM_BLUE.format());
  EXPECT_EQ("RED GREEN", (CM_RED | CM_GREEN).format());
  EXPECT_EQ("GREEN BLUE", (CM_GREEN | CM_BLUE).format());
  EXPECT_EQ("RED GREEN BLUE", (CM_RED | CM_GREEN | CM_BLUE).format());
}

TEST(OptionSet, containsAllOf) {
  EXPECT_TRUE((CM_RED | CM_GREEN).contains(CM_RED));
  EXPECT_FALSE((CM_RED | CM_GREEN).contains(CM_BLUE));
  EXPECT_FALSE((CM_RED | CM_GREEN).contains(CM_RED | CM_BLUE));

  EXPECT_TRUE((CM_RED | CM_GREEN).containsAllOf(CM_RED));
  EXPECT_FALSE((CM_RED | CM_GREEN).containsAllOf(CM_BLUE));
  EXPECT_FALSE((CM_RED | CM_GREEN).containsAllOf(CM_RED | CM_BLUE));
}

TEST(OptionSet, intersect) {
  EXPECT_FALSE(CM_RED & CM_BLUE);
  EXPECT_EQ(CM_GREEN, (CM_RED | CM_GREEN) & (CM_GREEN | CM_BLUE));
}

TEST(OptionSet, containsAnyOf) {
  EXPECT_TRUE((CM_RED | CM_GREEN).containsAnyOf(CM_GREEN | CM_BLUE));
  EXPECT_FALSE((CM_RED | CM_GREEN).containsAnyOf(CM_BLUE));
}

TEST(OptionSet, containsNoneOf) {
  EXPECT_FALSE((CM_RED | CM_GREEN).containsNoneOf(CM_GREEN | CM_BLUE));
  EXPECT_TRUE(CM_RED.containsNoneOf(CM_GREEN | CM_BLUE));
}
