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

#include "common/stats/ExportedHistogramMap.h"
#include "common/stats/ExportedStatMap.h"
#include "common/stats/DynamicCounters.h"
#include <folly/Range.h>
#include <map>

namespace facebook { namespace stats {

class ServiceData {
 public:
  static ServiceData* get();

  ExportedStatMap* getStatMap() {
    static ExportedStatMap it;
    return &it;
  }
  ExportedHistogramMap* getHistogramMap() {
    static ExportedHistogramMap it;
    return &it;
  }
  std::map<std::string, int64_t> getCounters() const {
    return std::map<std::string, int64_t>{};
  }
  void getCounters(std::map<std::string, int64_t>&) const {}
  long getCounter(folly::StringPiece) const { return 0; };
  long clearCounter(folly::StringPiece) { return 0; };
  void setUseOptionsAsFlags(bool) {}
  int64_t incrementCounter(folly::StringPiece, int64_t amount = 1) {
    return amount;
  }
  void setCounter(folly::StringPiece, uint32_t) {}
  DynamicCounters *getDynamicCounters() {
    return &counters_;
  }
  void addStatValue(
      folly::StringPiece key,
      int64_t value,
      stats::ExportType exportType) {}

 private:
  DynamicCounters counters_;
};

}

extern stats::ServiceData* fbData;

}
