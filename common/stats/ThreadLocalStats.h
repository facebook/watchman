// Copyright 2004-present Facebook. All Rights Reserved.
#pragma once

#include <chrono>
#include <folly/Range.h>
#include <folly/Synchronized.h>
#include <folly/stats/TimeseriesHistogram.h>

#include "common/stats/ExportType.h"

namespace facebook { namespace stats {

class TLStatsThreadSafe {};

template <class LockTraits>
class ThreadLocalStatsT {
 public:
  // TODO: This stub implementation just uses folly::Synchronized, and is not
  // actually thread-local for now.
  class TLHistogram {
   public:
     template <typename... ExportArgs>
     TLHistogram(ThreadLocalStatsT *container, folly::StringPiece name,
                 size_t bucketWidth, int64_t minValue, int64_t maxValue,
                 ExportArgs... exports)
         : histogram_(
               folly::in_place, bucketWidth, minValue, maxValue,
               folly::MultiLevelTimeSeries<int64_t>{
                   /* num buckets */ 60,
                   {
                       std::chrono::seconds{60}, std::chrono::seconds{600},
                       std::chrono::seconds{3600}, std::chrono::seconds{0},
                   }}) {
       // We don't handle setting up exports for now.
    }

    void addValue(int64_t value) {
      // The default TimeseriesHistogram clock does not implement now;
      // set the time using steady_clock here.
      auto now = std::chrono::duration_cast<
          folly::TimeseriesHistogram<int64_t>::Duration>(
          std::chrono::steady_clock::now().time_since_epoch());
      histogram_->addValue(now, value);
    }

  private:
    folly::Synchronized<folly::TimeseriesHistogram<int64_t>> histogram_;
  };

  void aggregate() {}
};

} // stats
} // facebook
