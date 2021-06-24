/* Copyright (c) Facebook, Inc. and its affiliates.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman/watchman_perf.h"

#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>
#include "watchman/WatchmanConfig.h"
#include "watchman/thirdparty/jansson/jansson.h"

using namespace watchman;

TEST(Perf, thread_shutdown) {
  cfg_set_arg("perf_logger_command", json_array({w_string_to_json("echo")}));
  SCOPE_EXIT {
    // We must call perf_shutdown() before cfg_shutdown(),
    // since the perf thread accesses configuration data.
    perf_shutdown();
    cfg_shutdown();
  };

  watchman_perf_sample sample("test");
  sample.force_log();
  auto logged = sample.finish();
  EXPECT_TRUE(logged);
  sample.log();
}

namespace {
json_ref make_sample(int i) {
  return json_object({std::make_pair("value", json_integer(i))});
}
} // namespace

TEST(Perf, sample_batches_are_limited_to_batch_size) {
  auto samples = json_array({
      make_sample(1),
      make_sample(2),
      make_sample(3),
      make_sample(4),
      make_sample(5),
  });

  std::vector<std::vector<std::string>> calls;

  processSamples(
      1000,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        calls.push_back(std::move(samples));
      },
      [&](std::string) {
        throw std::runtime_error("unexpected stdin callback");
      });

  ASSERT_EQ(2, calls.size());
  ASSERT_EQ(4, calls[0].size());
  EXPECT_EQ("{\"value\": 1}", calls[0][0]);
  EXPECT_EQ("{\"value\": 2}", calls[0][1]);
  ASSERT_EQ(1, calls[1].size());
  EXPECT_EQ("{\"value\": 5}", calls[1][0]);
}

TEST(Perf, sample_batches_are_limited_if_total_size_exceeds_argv_limit) {
  auto samples = json_array({
      make_sample(1),
      make_sample(2),
      make_sample(3),
      make_sample(4),
      make_sample(5),
  });

  std::vector<std::vector<std::string>> calls;

  processSamples(
      20,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        calls.push_back(std::move(samples));
      },
      [&](std::string) {
        throw std::runtime_error("unexpected stdin callback");
      });

  ASSERT_EQ(3, calls.size());
  ASSERT_EQ(2, calls[0].size());
  EXPECT_EQ("{\"value\": 1}", calls[0][0]);
  EXPECT_EQ("{\"value\": 2}", calls[0][1]);
  ASSERT_EQ(2, calls[1].size());
  EXPECT_EQ("{\"value\": 3}", calls[1][0]);
  EXPECT_EQ("{\"value\": 4}", calls[1][1]);
  ASSERT_EQ(1, calls[2].size());
  EXPECT_EQ("{\"value\": 5}", calls[2][0]);
}

TEST(Perf, large_samples_are_passed_in_stdin) {
  auto samples = json_array({
      make_sample(1),
      make_sample(2),
  });

  std::vector<std::vector<std::string>> arg_calls;
  std::vector<std::string> stdin_calls;

  processSamples(
      5,
      4,
      samples,
      [&](std::vector<std::string> samples) {
        arg_calls.push_back(std::move(samples));
      },
      [&](std::string stdin) { stdin_calls.push_back(std::move(stdin)); });

  ASSERT_EQ(0, arg_calls.size());
  ASSERT_EQ(2, stdin_calls.size());
  EXPECT_EQ("{\"value\": 1}", stdin_calls[0]);
  EXPECT_EQ("{\"value\": 2}", stdin_calls[1]);
}
