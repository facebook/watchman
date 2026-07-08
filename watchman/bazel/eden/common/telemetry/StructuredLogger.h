#pragma once

#include <optional>

#include "eden/common/telemetry/DynamicEvent.h"
#include "eden/common/telemetry/LogEvent.h"

namespace facebook::eden {

class StructuredLogger {
 public:
  virtual ~StructuredLogger() = default;

  virtual DynamicEvent populateDefaultFields(std::optional<const char*> type) {
    DynamicEvent event;
    if (type) {
      event.addString("type", *type);
    }
    return event;
  }

  void logEvent(const TypedEvent& typedEvent) {
    auto event = populateDefaultFields(typedEvent.getType());
    typedEvent.populate(event);
  }
};

} // namespace facebook::eden
