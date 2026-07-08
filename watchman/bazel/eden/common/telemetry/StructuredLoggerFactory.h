#pragma once

#include <memory>
#include <string>
#include <utility>

#include "eden/common/telemetry/ScribeLogger.h"
#include "eden/common/telemetry/SessionInfo.h"

namespace facebook::eden {

template <typename Logger, typename StatsPtr>
std::shared_ptr<Logger> makeDefaultStructuredLogger(
    std::string,
    std::string,
    SessionInfo sessionInfo,
    StatsPtr) {
  return std::make_shared<Logger>(
      std::make_shared<ScribeLogger>(), std::move(sessionInfo));
}

} // namespace facebook::eden
