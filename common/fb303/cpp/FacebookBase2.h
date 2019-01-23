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

#include <time.h>

#include <common/fb303/if/gen-cpp2/FacebookService.h>
#include <folly/small_vector.h>

namespace folly {
class EventBaseManager;
}

namespace facebook { namespace fb303 {

enum ThriftFuncAction {
  FIRST_ACTION = 0,
  READ = FIRST_ACTION,
  WRITE,
  PROCESS,
  BYTES_READ,
  BYTES_WRITTEN,
  LAST_ACTION
};

class FacebookBase2 : virtual public cpp2::FacebookServiceSvIf {
  time_t startTime;
public:
  explicit FacebookBase2(const char*) {
    startTime = time(nullptr);
  }

  void setEventBaseManager(folly::EventBaseManager*) {}

  int64_t aliveSince() override {
    // crude implementation because QsfpCache depends on it
    return (uint64_t) startTime;
  }

  int64_t getPid() override {
    return getpid();
  }

  void exportThriftFuncHist(
      const std::string& /*funcName*/,
      ThriftFuncAction /*action*/,
      folly::small_vector<int> /*percentiles*/,
      int64_t /*bucketSize*/,
      int64_t /*min*/,
      int64_t /*max*/) {
  }

  void exportThriftFuncHist(
      const std::string& funcName,
      ThriftFuncAction action,
      int percentile,
      int64_t bucketSize,
      int64_t min,
      int64_t max) {
    exportThriftFuncHist(
        funcName,
        action,
        folly::small_vector<int>({percentile}),
        bucketSize,
        min,
        max);
  }
};

}}
