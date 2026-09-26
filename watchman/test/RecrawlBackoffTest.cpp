/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/root/RecrawlBackoff.h"

#include <folly/portability/GTest.h>
#include <chrono>

using namespace std::chrono;
using namespace watchman;

TEST(RecrawlBackoffTest, free_attempts_return_zero) {
  RecrawlBackoff backoff;
  auto t0 = steady_clock::time_point{seconds(1000)};

  // 1st rapid recrawl
  auto now1 = t0 + milliseconds(500);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now1, t0));
  EXPECT_EQ(1u, backoff.consecutiveRapidCount());

  // 2nd rapid recrawl
  auto finish1 = now1 + milliseconds(100);
  auto now2 = finish1 + milliseconds(500);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now2, finish1));
  EXPECT_EQ(2u, backoff.consecutiveRapidCount());

  // 3rd rapid recrawl (last free attempt)
  auto finish2 = now2 + milliseconds(100);
  auto now3 = finish2 + milliseconds(500);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now3, finish2));
  EXPECT_EQ(3u, backoff.consecutiveRapidCount());
}

TEST(RecrawlBackoffTest, doubling_after_free_attempts) {
  RecrawlBackoff backoff;
  auto finish = steady_clock::time_point{seconds(1000)};

  // 3 free attempts
  for (int i = 0; i < 3; ++i) {
    auto now = finish + milliseconds(500);
    EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now, finish));
    finish = now + milliseconds(100);
  }
  EXPECT_EQ(3u, backoff.consecutiveRapidCount());

  // 4th attempt: consecutive=4, delay = 100 * 2^0 = 100ms
  auto now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(100), backoff.onRecrawl(now, finish));
  EXPECT_EQ(4u, backoff.consecutiveRapidCount());
  finish = now + milliseconds(100);

  // 5th attempt: consecutive=5, delay = 100 * 2^1 = 200ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(200), backoff.onRecrawl(now, finish));
  EXPECT_EQ(5u, backoff.consecutiveRapidCount());
  finish = now + milliseconds(100);

  // 6th attempt: consecutive=6, delay = 100 * 2^2 = 400ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(400), backoff.onRecrawl(now, finish));
  EXPECT_EQ(6u, backoff.consecutiveRapidCount());
  finish = now + milliseconds(100);

  // 7th attempt: consecutive=7, delay = 100 * 2^3 = 800ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(800), backoff.onRecrawl(now, finish));
  EXPECT_EQ(7u, backoff.consecutiveRapidCount());
}

TEST(RecrawlBackoffTest, cap_at_max_delay) {
  RecrawlBackoff::Options options;
  options.freeAttempts = 0;
  options.initialDelay = milliseconds(100);
  options.maxDelay = milliseconds(700);
  RecrawlBackoff backoff{options};

  auto finish = steady_clock::time_point{seconds(1000)};

  // consecutive=1: 100 * 2^0 = 100ms
  auto now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(100), backoff.onRecrawl(now, finish));
  finish = now + milliseconds(100);

  // consecutive=2: 100 * 2^1 = 200ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(200), backoff.onRecrawl(now, finish));
  finish = now + milliseconds(100);

  // consecutive=3: 100 * 2^2 = 400ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(400), backoff.onRecrawl(now, finish));
  finish = now + milliseconds(100);

  // consecutive=4: 100 * 2^3 = 800ms -> capped at 700ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(700), backoff.onRecrawl(now, finish));
  finish = now + milliseconds(100);

  // consecutive=5: capped at 700ms
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds(700), backoff.onRecrawl(now, finish));
}

TEST(RecrawlBackoffTest, reset_after_non_rapid_gap) {
  RecrawlBackoff backoff;
  auto finish = steady_clock::time_point{seconds(1000)};

  // Reach backoff threshold
  for (int i = 0; i < 5; ++i) {
    auto now = finish + milliseconds(500);
    backoff.onRecrawl(now, finish);
    finish = now + milliseconds(100);
  }
  EXPECT_EQ(5u, backoff.consecutiveRapidCount());

  // Non-rapid gap: elapsed >= rapidWindow (10s)
  auto now = finish + milliseconds(15000);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now, finish));
  EXPECT_EQ(0u, backoff.consecutiveRapidCount());

  // Next recrawl is rapid again, but starts from consecutive=1
  finish = now + milliseconds(100);
  now = finish + milliseconds(500);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now, finish));
  EXPECT_EQ(1u, backoff.consecutiveRapidCount());
}

TEST(RecrawlBackoffTest, max_delay_zero_disables) {
  RecrawlBackoff::Options options;
  options.maxDelay = milliseconds::zero();
  RecrawlBackoff backoff{options};

  auto finish = steady_clock::time_point{seconds(1000)};
  for (int i = 0; i < 10; ++i) {
    auto now = finish + milliseconds(100);
    EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now, finish));
    EXPECT_EQ(0u, backoff.consecutiveRapidCount());
    finish = now + milliseconds(100);
  }
}

TEST(RecrawlBackoffTest, last_crawl_finish_zero_counts_as_non_rapid) {
  RecrawlBackoff backoff;
  auto now = steady_clock::time_point{seconds(1000)};

  // lastCrawlFinish == time_point{} (initial state before any crawl finished)
  EXPECT_EQ(
      milliseconds::zero(),
      backoff.onRecrawl(now, steady_clock::time_point{}));
  EXPECT_EQ(0u, backoff.consecutiveRapidCount());

  // Rapid recrawl sets count to 1
  auto finish = now + milliseconds(100);
  now = finish + milliseconds(200);
  EXPECT_EQ(milliseconds::zero(), backoff.onRecrawl(now, finish));
  EXPECT_EQ(1u, backoff.consecutiveRapidCount());

  // A call with lastCrawlFinish == time_point{} resets consecutive to 0
  EXPECT_EQ(
      milliseconds::zero(),
      backoff.onRecrawl(now, steady_clock::time_point{}));
  EXPECT_EQ(0u, backoff.consecutiveRapidCount());
}

TEST(RecrawlBackoffTest, very_large_consecutive_counts_do_not_overflow) {
  RecrawlBackoff backoff;
  auto finish = steady_clock::time_point{seconds(1000)};

  for (uint32_t i = 0; i < 10000; ++i) {
    auto now = finish + milliseconds(100);
    auto delay = backoff.onRecrawl(now, finish);
    if (i < 3) {
      EXPECT_EQ(milliseconds::zero(), delay);
    } else {
      EXPECT_LE(delay, RecrawlBackoff::Options{}.maxDelay);
      EXPECT_GE(delay, milliseconds(100));
    }
    finish = now + milliseconds(50);
  }
  EXPECT_EQ(10000u, backoff.consecutiveRapidCount());

  // Delay is safely capped at default maxDelay with no overflow
  auto now = finish + milliseconds(100);
  EXPECT_EQ(
      RecrawlBackoff::Options{}.maxDelay, backoff.onRecrawl(now, finish));
}
