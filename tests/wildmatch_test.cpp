/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"

#include "thirdparty/jansson/jansson.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include "thirdparty/tap.h"

#define WILDMATCH_TEST_JSON_FILE "tests/wildmatch_test.json"

static void run_test(json_t *test_case_data)
{
  int wildmatch_should_succeed;
  int wildmatch_flags;
  char *text_to_match;
  char *pattern_to_use;
  int wildmatch_succeeded;

  json_error_t error;
  if (json_unpack_ex(
        test_case_data,
        &error,
        0,
        "[b,i,s,s]",
        &wildmatch_should_succeed,
        &wildmatch_flags,
        &text_to_match,
        &pattern_to_use) == -1) {
    fail(
      "Error decoding JSON: %s (source=%s, line=%d, col=%d)\n",
      error.text,
      error.source,
      error.line,
      error.column);
    return;
  }

  wildmatch_succeeded =
    wildmatch(pattern_to_use, text_to_match, wildmatch_flags, 0) == WM_MATCH;
  if (wildmatch_should_succeed) {
    ok(
      wildmatch_succeeded,
      "Pattern [%s] should match text [%s] with flags %d",
      pattern_to_use,
      text_to_match,
      wildmatch_flags);
  } else {
    ok(
      !wildmatch_succeeded,
      "Pattern [%s] should not match text [%s] with flags %d",
      pattern_to_use,
      text_to_match,
      wildmatch_flags);
  }
}

int main(int, char**) {
  FILE *test_cases_file;
  json_error_t error;
  size_t num_tests;
  size_t index;

  test_cases_file = fopen(WILDMATCH_TEST_JSON_FILE, "r");
  if (!test_cases_file) {
    test_cases_file = fopen("watchman/" WILDMATCH_TEST_JSON_FILE, "r");
  }
  if (!test_cases_file) {
    diag("Couldn't open %s: %s\n", WILDMATCH_TEST_JSON_FILE, strerror(errno));
    abort();
  }
  auto test_cases = json_loadf(test_cases_file, 0, &error);
  if (!test_cases) {
    diag(
      "Error decoding JSON: %s (source=%s, line=%d, col=%d)\n",
      error.text,
      error.source,
      error.line,
      error.column);
    abort();
  }
  if (fclose(test_cases_file) != 0) {
    diag("Error closing %s: %s\n", WILDMATCH_TEST_JSON_FILE, strerror(errno));
    abort();
  }
  if (!json_is_array(test_cases)) {
    diag("Expected JSON in %s to be an array\n", WILDMATCH_TEST_JSON_FILE);
    abort();
  }
  num_tests = json_array_size(test_cases);
  plan_tests((unsigned int)num_tests);
  for (index = 0; index < num_tests; index++) {
    auto test_case_data = json_array_get(test_cases, index);
    run_test(test_case_data);
  }
  return exit_status();
}
