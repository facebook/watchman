/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/Range.h>
#include <cstdint>
#include "common/stats/ExportedHistogramMapImpl.h"
#include "common/stats/ExportedStatMap.h"
#include "common/stats/ExportedStatMapImpl.h"

namespace facebook { namespace stats {

class ThreadCachedServiceData {
public:
  class ThreadLocalStatsMap {
  };

  class TLTimeseries {
  public:
    TLTimeseries(ThreadLocalStatsMap*, folly::StringPiece,
                 ExportType, ExportType = ExportType()) {}
    void addValue(int64_t) {}
  };

  class TLHistogram {
  public:
    TLHistogram(ThreadLocalStatsMap*, folly::StringPiece, int, int, int) {}
    TLHistogram(
        ThreadLocalStatsMap*, folly::StringPiece, int, int, int,
        facebook::stats::ExportType, int, int) {}
    void addValue(int64_t) {}
    void addRepeatedValue(int64_t /*value*/, int64_t /*nsamples*/) {}
  };

  class TLCounter {
    public:
      TLCounter(ThreadLocalStatsMap*, folly::StringPiece) {}
      void incrementValue(int64_t) {}

  };

  static ThreadCachedServiceData* get() {
    static ThreadCachedServiceData it;
    return &it;
  }
  ThreadLocalStatsMap* getThreadStats() {
    static ThreadLocalStatsMap it;
    return &it;
  }

  void publishStats() {}
  void startPublishThread(std::chrono::milliseconds /*interval*/) {}

  void addStatValue(const std::string& key, int64_t value = 1) {}
  void addStatValue(
    const std::string& key,
    int64_t value,
    stats::ExportType exportType) {}
  int64_t setCounter(const std::string& key, int64_t value) { return 0; }
  void clearCounter(const std::string& key) {}
};

}} // unnamed facebook::stats

namespace facebook {
/**
 * convenience function; returns the ThreadCachedServiceData singleton
 * which wraps ServiceData::get().
 */
inline stats::ThreadCachedServiceData& tcData() {
  return *(stats::ThreadCachedServiceData::get());
}
}  // unnamed facebook
