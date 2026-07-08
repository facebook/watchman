#pragma once

#include <memory>
#include <utility>

#include "eden/common/telemetry/ScribeLogger.h"
#include "eden/common/telemetry/SessionInfo.h"
#include "eden/common/telemetry/StructuredLogger.h"

namespace facebook::eden {

class ScubaStructuredLogger : public StructuredLogger {
 public:
  ScubaStructuredLogger(
      std::shared_ptr<ScribeLogger> scribeLogger,
      SessionInfo sessionInfo)
      : scribeLogger_(std::move(scribeLogger)),
        sessionInfo_(std::move(sessionInfo)) {}

 protected:
  std::shared_ptr<ScribeLogger> scribeLogger_;
  SessionInfo sessionInfo_;
};

} // namespace facebook::eden
