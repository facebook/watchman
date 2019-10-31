/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"

#include <folly/Conv.h>
#include <folly/portability/GTest.h>
#include "thirdparty/jansson/jansson.h"
#include "thirdparty/wildmatch/wildmatch.h"

#define WILDMATCH_TEST_JSON_FILE "tests/wildmatch_test.json"

static void run_test(const json_ref& test_case_data) {
  auto wildmatch_should_succeed = test_case_data.at(0).asBool();
  auto wildmatch_flags = test_case_data.at(1).asInt();
  auto text_to_match = json_string_value(test_case_data.at(2));
  auto pattern_to_use = json_string_value(test_case_data.at(3));

  auto wildmatch_succeeded =
      wildmatch(pattern_to_use, text_to_match, wildmatch_flags, nullptr) ==
      WM_MATCH;
  EXPECT_EQ(wildmatch_succeeded, wildmatch_should_succeed)
      << "Pattern [" << pattern_to_use << "] matching text [" << text_to_match
      << "] with flags " << wildmatch_flags;
}

TEST(WildMatch, tests) {
  FILE* test_cases_file;
  json_error_t error;
  size_t num_tests;
  size_t index;

  test_cases_file = fopen(WILDMATCH_TEST_JSON_FILE, "r");
#ifdef WATCHMAN_TEST_SRC_DIR
  if (!test_cases_file) {
    test_cases_file =
        fopen(WATCHMAN_TEST_SRC_DIR "/" WILDMATCH_TEST_JSON_FILE, "r");
  }
#endif
  if (!test_cases_file) {
    test_cases_file = fopen("watchman/" WILDMATCH_TEST_JSON_FILE, "r");
  }
  if (!test_cases_file) {
    throw std::runtime_error(folly::to<std::string>(
        "Couldn't open ", WILDMATCH_TEST_JSON_FILE, ": ", strerror(errno)));
  }
  auto test_cases = json_loadf(test_cases_file, 0, &error);
  if (!test_cases) {
    throw std::runtime_error(folly::to<std::string>(
        "Error decoding JSON: ",
        error.text,
        " (source=",
        error.source,
        ", line=",
        error.line,
        ", col=",
        error.column,
        ")"));
  }
  EXPECT_EQ(fclose(test_cases_file), 0);
  EXPECT_TRUE(test_cases.isArray())
      << "Expected JSON in " << WILDMATCH_TEST_JSON_FILE << "  to be an array";

  num_tests = json_array_size(test_cases);
  for (index = 0; index < num_tests; index++) {
    auto test_case_data = json_array_get(test_cases, index);
    run_test(test_case_data);
  }
}
