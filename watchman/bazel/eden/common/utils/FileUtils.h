#pragma once

#ifdef _WIN32
#include <windows.h>

#include <optional>

struct REPARSE_DATA_BUFFER;

namespace facebook::eden {

inline std::optional<REPARSE_DATA_BUFFER*> getReparseData(HANDLE) {
  return std::nullopt;
}

} // namespace facebook::eden
#endif
