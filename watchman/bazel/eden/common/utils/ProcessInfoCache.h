#pragma once

#include <folly/portability/SysTypes.h>

#include <string>
#include <utility>

namespace facebook::eden {

struct ProcessInfo {
  pid_t pid{0};
  std::string name;
};

class ProcessInfoHandle {
 public:
  ProcessInfoHandle() = default;

  explicit ProcessInfoHandle(ProcessInfo info) : info_(std::move(info)) {}

  const ProcessInfo& get() const {
    return info_;
  }

 private:
  ProcessInfo info_;
};

class ProcessInfoCache {
 public:
  ProcessInfoHandle lookup(pid_t pid) {
    return ProcessInfoHandle(ProcessInfo{pid, std::to_string(pid)});
  }

  static std::string cleanProcessCommandline(std::string command) {
    return command;
  }
};

} // namespace facebook::eden
