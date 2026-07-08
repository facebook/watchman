#pragma once

#include "eden/common/telemetry/StatsGroup.h"

namespace facebook::fb303 {

class QuantileStatMap {
 public:
  void flushAll() {}
};

class ServiceData {
 public:
  static ServiceData* get() {
    static ServiceData data;
    return &data;
  }

  QuantileStatMap* getQuantileStatMap() {
    return &map_;
  }

 private:
  QuantileStatMap map_;
};

} // namespace facebook::fb303

namespace facebook::eden {

class TelemetryStats : public StatsGroupBase {};

} // namespace facebook::eden
