// Copyright (c) 2007-present, Facebook, Inc.
//
// High resolution timers for wall clock and CPU time.

#ifndef COMMON_TIME_TIME_H
#define COMMON_TIME_TIME_H

#include <ctime>
#include <chrono>

using namespace std::chrono;

namespace facebook {

class WallClockUtil {
 public:
  // ----------------  time in seconds  ---------------
  static time_t NowInSecFast() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }
};

}  // !namespace facebook

#endif  // !COMMON_TIME_TIME_H
