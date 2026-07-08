#pragma once

#include "eden/common/telemetry/DynamicEvent.h"

namespace facebook::eden {

struct TypedEvent {
  virtual ~TypedEvent() = default;
  virtual void populate(DynamicEvent& event) const = 0;
  virtual const char* getType() const = 0;
};

} // namespace facebook::eden
