#pragma once

#include <chrono>

namespace facebook::eden {

class StatsGroupBase {
 public:
  class Counter {
   public:
    void addValue(double) {}
  };

  class Duration {
   public:
    template <typename Rep, typename Period>
    void addDuration(std::chrono::duration<Rep, Period>) {}
  };
};

} // namespace facebook::eden
