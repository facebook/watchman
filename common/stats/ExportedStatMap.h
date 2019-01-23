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
#include <chrono>

#include "common/stats/ExportType.h"

namespace facebook {

class SpinLock {
};

class SpinLockHolder {
public:
  explicit SpinLockHolder(SpinLock*) {}
};

namespace stats {

struct ExportedStat {
  void addValue(std::chrono::seconds, int64_t) {}
  void addValue(std::chrono::seconds::rep, uint64_t) {}
  void addValueLocked(std::chrono::seconds::rep, uint64_t) {}
  int numLevels() {return 1;}
  int getSum(int level) {return 0;}
  int sum(int level) {return 0;}
};

class ExportedStatMap {
public:
  class LockAndStatItem {
  public:
    std::shared_ptr<SpinLock> first;
    std::shared_ptr<ExportedStat> second;
  };
  LockAndStatItem getLockAndStatItem(folly::StringPiece,
                                     const ExportType* = nullptr) {
    static LockAndStatItem it = {
      std::make_shared<SpinLock>(), std::make_shared<ExportedStat>()
    };
    return it;
  }

  class LockableStat : public ExportedStat {
  };

  LockableStat getLockableStat(folly::StringPiece,
                               const ExportType* = nullptr) {
    static LockableStat stat;
    return stat;
  }

  std::shared_ptr<ExportedStat> getLockedStatPtr(folly::StringPiece name) {
      return std::make_shared<ExportedStat>();
  }

  std::shared_ptr<ExportedStat> getStatPtr(folly::StringPiece name) {
      return std::make_shared<ExportedStat>();
  }
};

}}
