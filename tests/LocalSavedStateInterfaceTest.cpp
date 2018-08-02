/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "saved_state/LocalSavedStateInterface.h"
#include "thirdparty/jansson/jansson.h"
#include "thirdparty/tap.h"
#include "watchman.h"

using namespace watchman;

void expect_query_parse_error(
    const json_ref& config,
    const char* expectedError) {
  try {
    LocalSavedStateInterface interface(config, nullptr);
    ok(false, "expected to throw %s", expectedError);
  } catch (const QueryParseError& error) {
    ok(true, "QueryParseError expected");
    ok(!strcmp(error.what(), expectedError),
       "Expected error \"%s\" and observed \"%s\"",
       expectedError,
       error.what());
  }
}

void test_max_commits() {
  auto localStoragePath = w_string_to_json(w_string("/absolute/path"));
  auto project = w_string_to_json("foo");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", project},
                   {"max-commits", w_string_to_json("string")}}),
      "failed to parse query: 'max-commits' must be an integer");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", project},
                   {"max-commits", json_integer(0)}}),
      "failed to parse query: 'max-commits' must be a positive integer");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", project},
                   {"max-commits", json_integer(-1)}}),
      "failed to parse query: 'max-commits' must be a positive integer");
  LocalSavedStateInterface interface(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", project},
                   {"max-commits", json_integer(1)}}),
      nullptr);
  ok(true, "expected constructor to succeed");
}

void test_localStoragePath() {
  auto project = w_string_to_json(w_string("foo"));
  expect_query_parse_error(
      json_object({{"project", project}}),
      "failed to parse query: 'local-storage-path' must be present in saved state config");
  expect_query_parse_error(
      json_object(
          {{"project", project}, {"local-storage-path", json_integer(5)}}),
      "failed to parse query: 'local-storage-path' must be a string");
  expect_query_parse_error(
      json_object({{"project", project},
                   {"local-storage-path", w_string_to_json("relative/path")}}),
      "failed to parse query: 'local-storage-path' must be an absolute path");
  LocalSavedStateInterface interface(
      json_object({{"project", project},
                   {"local-storage-path", w_string_to_json("/absolute/path")}}),
      nullptr);
  ok(true, "expected constructor to succeed");
}

void test_project() {
  auto localStoragePath = w_string_to_json("/absolute/path");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath}}),
      "failed to parse query: 'project' must be present in saved state config");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", json_integer(5)}}),
      "failed to parse query: 'project' must be a string");
  expect_query_parse_error(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", w_string_to_json("/absolute/path")}}),
      "failed to parse query: 'project' must be a relative path");
  LocalSavedStateInterface interface(
      json_object({{"local-storage-path", localStoragePath},
                   {"project", w_string_to_json("foo")}}),
      nullptr);
  ok(true, "expected constructor to succeed");
}

void test_invalid_localStoragePath() {}

int main(int, char**) {
  plan_tests(21);
  test_max_commits();
  test_localStoragePath();
  test_project();

  return exit_status();
}
