#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace facebook::eden {

class DynamicEvent {
 public:
  using Value = std::variant<bool, int64_t, std::string>;

  void addBool(const std::string& key, bool value) {
    fields_[key] = value;
  }

  void addInt(const std::string& key, int64_t value) {
    fields_[key] = value;
  }

  void addString(const std::string& key, std::string value) {
    fields_[key] = std::move(value);
  }

 private:
  std::unordered_map<std::string, Value> fields_;
};

} // namespace facebook::eden
