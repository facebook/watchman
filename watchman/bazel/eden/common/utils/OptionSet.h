#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace facebook::eden {

template <typename Derived, typename Storage>
class OptionSet {
 public:
  using UnderlyingType = Storage;
  using NameTable = std::vector<std::pair<Derived, const char*>>;

  constexpr OptionSet() = default;
  constexpr explicit OptionSet(Storage value) : value_(value) {}

  static constexpr Derived raw(Storage value) {
    Derived result;
    result.value_ = value;
    return result;
  }

  constexpr Storage asRaw() const {
    return value_;
  }

  constexpr bool contains(Derived flag) const {
    return (value_ & flag.asRaw()) == flag.asRaw();
  }

  constexpr bool containsNoneOf(Derived flags) const {
    return (value_ & flags.asRaw()) == 0;
  }

  constexpr void set(Derived flags) {
    value_ |= flags.asRaw();
  }

  constexpr void unset(Derived flags) {
    value_ &= ~flags.asRaw();
  }

  constexpr explicit operator bool() const {
    return value_ != 0;
  }

  std::string format() const {
    std::string result;
    Storage remaining = value_;
    for (const auto& item : Derived::table) {
      if (contains(item.first)) {
        if (!result.empty()) {
          result += "|";
        }
        result += item.second;
        remaining &= ~item.first.asRaw();
      }
    }
    if (remaining) {
      if (!result.empty()) {
        result += "|";
      }
      result += std::to_string(static_cast<uint64_t>(remaining));
    }
    if (result.empty()) {
      return "0";
    }
    return result;
  }

  friend constexpr Derived operator|(OptionSet lhs, OptionSet rhs) {
    return Derived::raw(lhs.value_ | rhs.value_);
  }

  friend constexpr Derived operator&(OptionSet lhs, OptionSet rhs) {
    return Derived::raw(lhs.value_ & rhs.value_);
  }

  friend constexpr Derived operator~(OptionSet value) {
    return Derived::raw(~value.value_);
  }

  friend constexpr Derived& operator|=(OptionSet& lhs, OptionSet rhs) {
    lhs.value_ |= rhs.value_;
    return static_cast<Derived&>(lhs);
  }

  friend constexpr Derived& operator&=(OptionSet& lhs, OptionSet rhs) {
    lhs.value_ &= rhs.value_;
    return static_cast<Derived&>(lhs);
  }

  friend constexpr bool operator==(OptionSet lhs, OptionSet rhs) {
    return lhs.value_ == rhs.value_;
  }

  friend constexpr bool operator!=(OptionSet lhs, OptionSet rhs) {
    return lhs.value_ != rhs.value_;
  }

 private:
  Storage value_{0};
};

} // namespace facebook::eden
