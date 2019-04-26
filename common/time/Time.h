// Copyright (c) 2007-present, Facebook, Inc.
//
// High resolution timers for wall clock and CPU time.

#ifndef COMMON_TIME_TIME_H
#define COMMON_TIME_TIME_H

#include <ctime>

namespace facebook {

class WallClockUtil {
 public:
  // ----------------  time in seconds  ---------------
  static time_t NowInSecFast() {
    return 0;
  }
};

}  // !namespace facebook

#endif  // !COMMON_TIME_TIME_H
