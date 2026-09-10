/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/telemetry/LogEvent.h"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <thread>
#include <vector>

#include <folly/portability/GTest.h>

namespace {

int64_t nextCounterValue(int64_t value, int64_t samplingRate) {
  return value == samplingRate ? 1 : value + 1;
}

TEST(LogEventTest, incrementsEventCounter) {
  const auto [samplingRate, firstCount] =
      watchman::getLogEventCounters(watchman::DroppedType);
  const auto [nextSamplingRate, secondCount] =
      watchman::getLogEventCounters(watchman::DroppedType);

  EXPECT_GT(samplingRate, 0);
  EXPECT_EQ(samplingRate, nextSamplingRate);
  EXPECT_EQ(nextCounterValue(firstCount, samplingRate), secondCount);
}

TEST(LogEventTest, sharesFallbackCounterForInvalidEventTypes) {
  const auto sentinel = watchman::LogEventTypeCount;
  const auto beyondSentinel =
      static_cast<watchman::LogEventType>(watchman::LogEventTypeCount + 1);

  const auto [samplingRate, firstCount] =
      watchman::getLogEventCounters(sentinel);
  const auto [nextSamplingRate, secondCount] =
      watchman::getLogEventCounters(beyondSentinel);

  EXPECT_GT(samplingRate, 0);
  EXPECT_EQ(samplingRate, nextSamplingRate);
  EXPECT_EQ(nextCounterValue(firstCount, samplingRate), secondCount);
}

TEST(LogEventTest, supportsConcurrentEventCounterAccess) {
  constexpr size_t kThreadCount = 8;
  constexpr size_t kIterations = 1024;

  std::atomic<size_t> invalidResults{0};
  std::barrier startBarrier{static_cast<std::ptrdiff_t>(kThreadCount)};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
    threads.emplace_back([threadIndex, &invalidResults, &startBarrier] {
      startBarrier.arrive_and_wait();
      for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const auto type = static_cast<watchman::LogEventType>(
            (threadIndex + iteration) % watchman::LogEventTypeCount);
        const auto [samplingRate, eventCount] =
            watchman::getLogEventCounters(type);
        if (samplingRate <= 0 || eventCount <= 0 || eventCount > samplingRate) {
          ++invalidResults;
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(0, invalidResults.load());
}

} // namespace
