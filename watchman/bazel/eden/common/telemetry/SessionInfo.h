#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace facebook::eden {

struct UserInfo {
  static UserInfo lookup() {
    return UserInfo{};
  }
};

struct SessionInfo {
  using Field = std::variant<std::string, uint64_t>;

  std::string hostname;
  std::string appVersion;
  std::vector<std::pair<std::string, Field>> fbInfo;
};

inline std::string getHostname() {
  return "";
}

inline SessionInfo makeSessionInfo(
    UserInfo,
    std::string hostname,
    std::string appVersion) {
  SessionInfo info;
  info.hostname = std::move(hostname);
  info.appVersion = std::move(appVersion);
  return info;
}

} // namespace facebook::eden
