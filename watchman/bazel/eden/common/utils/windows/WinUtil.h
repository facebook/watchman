#pragma once

#include <folly/portability/SysTypes.h>

namespace facebook::eden {

inline pid_t getPeerProcessID(int) {
  return 0;
}

} // namespace facebook::eden
