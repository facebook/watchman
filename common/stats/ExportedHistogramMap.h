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

#include "common/stats/ExportedStatMap.h"

namespace facebook { namespace stats {

class ExportedHistogram {
public:
  ExportedHistogram() {}
  ExportedHistogram(int, int, size_t) {}
  void addValue(std::chrono::seconds, int, int64_t) {}
  int numLevels() {return 1;}
  int sum(int numLevels) {return 0;}
};

class ExportedHistogramMap {
public:
  class SpinLockGuard {
      /* no op */
  };

  struct LockAndHistogram {
    std::shared_ptr<SpinLock> first;
    std::shared_ptr<ExportedHistogram> second;
  };

  LockAndHistogram getOrCreateLockAndHistogram(folly::StringPiece,
                                                const ExportedHistogram*,
                                                bool* createdPtr = nullptr) {
    if (createdPtr) *createdPtr = true;
    return LockAndHistogram();
  }

  struct LockableHistogram : public ExportedHistogram {
    SpinLockGuard makeLockGuard() {return SpinLockGuard();}
    void addValueLocked(SpinLockGuard&, std::chrono::seconds::rep, int, uint64_t) {}
  };

  LockableHistogram getOrCreateLockableHistogram(folly::StringPiece,
                                                const ExportedHistogram*,
                                                bool* createdPtr = nullptr) {
    if (createdPtr) {
        *createdPtr = true;
    }
    return LockableHistogram();
  }
};

}}
