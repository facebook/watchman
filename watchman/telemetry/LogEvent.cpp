/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <array>
#include <atomic>

#include "watchman/WatchmanConfig.h"
#include "watchman/telemetry/LogEvent.h"

namespace watchman {

std::pair<int64_t, int64_t> getLogEventCounters(const LogEventType& type) {
  static std::array<std::atomic_int64_t, LogEventTypeCount> eventCounters{};
  static std::atomic_int64_t unknownEventCounter{};
  static const int64_t samplingRate =
      std::max<int64_t>(cfg_get_int("scribe-sampling-rate", 100), 1);

  const auto eventIndex = static_cast<size_t>(type);
  auto& eventCounter = eventIndex < eventCounters.size()
      ? eventCounters[eventIndex]
      : unknownEventCounter;
  auto eventCount =
      (eventCounter.fetch_add(1, std::memory_order_relaxed) + 1) % samplingRate;
  return std::make_pair(samplingRate, eventCount ? eventCount : samplingRate);
}

} // namespace watchman
