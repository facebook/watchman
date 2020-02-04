/* Copyright (c) Facebook, Inc. and its affiliates.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>
#include "thirdparty/jansson/jansson.h"

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
