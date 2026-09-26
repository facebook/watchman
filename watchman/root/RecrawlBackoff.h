/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace watchman {

// Throttles repeated recrawls of a root.
//
// A recrawl is "rapid" if it is requested within rapidWindow of the previous
// full crawl finishing. The first freeAttempts consecutive rapid recrawls run
// immediately. After that the delay starts at initialDelay and doubles for
// each further rapid recrawl, up to maxDelay. A recrawl that is not rapid
// resets the count. A maxDelay of zero disables backoff.
//
// Only used from the IO thread, so no locking is needed.
class RecrawlBackoff {
 public:
  struct Options {
    std::chrono::milliseconds rapidWindow{10000};
    uint32_t freeAttempts{3};
    std::chrono::milliseconds initialDelay{100};
    std::chrono::milliseconds maxDelay{10000};
  };

  RecrawlBackoff() = default;
  explicit RecrawlBackoff(Options options) : options_{options} {}

  std::chrono::milliseconds onRecrawl(
      std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::time_point lastCrawlFinish) {
    if (options_.maxDelay.count() == 0) {
      consecutiveRapid_ = 0;
      return std::chrono::milliseconds::zero();
    }

    if (lastCrawlFinish != std::chrono::steady_clock::time_point{} &&
        now - lastCrawlFinish < options_.rapidWindow) {
      ++consecutiveRapid_;
    } else {
      consecutiveRapid_ = 0;
    }

    if (consecutiveRapid_ <= options_.freeAttempts) {
      return std::chrono::milliseconds::zero();
    }

    uint32_t exponent = std::min<uint32_t>(
        consecutiveRapid_ - options_.freeAttempts - 1, 20);
    uint64_t multiplier = uint64_t{1} << exponent;
    uint64_t delayMs =
        static_cast<uint64_t>(options_.initialDelay.count()) * multiplier;
    uint64_t maxMs = static_cast<uint64_t>(options_.maxDelay.count());

    return std::chrono::milliseconds{std::min(delayMs, maxMs)};
  }

  uint32_t consecutiveRapidCount() const {
    return consecutiveRapid_;
  }

 private:
  Options options_;
  uint32_t consecutiveRapid_{0};
};

} // namespace watchman
