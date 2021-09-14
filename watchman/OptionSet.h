/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <assert.h>
#include <string.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace watchman {

/**
 * Typed wrapper around bit sets. Inspired by Swift's OptionSet.
 *
 * See OptionSetTest.cpp for example usage.
 */
template <typename Derived, typename Underlying>
class OptionSet {
  static_assert(std::is_integral_v<Underlying>);
  using zero_t = struct zero_t***;

 public:
  using UnderlyingType = Underlying;
  using NameTable = std::initializer_list<std::pair<Derived, std::string_view>>;

  constexpr OptionSet() = default;

  /**
   * Allows initialization from literal 0.
   */
  /* implicit */ constexpr OptionSet(zero_t) {}

  /* implicit */ constexpr OptionSet(Derived v) : value_{v.value_} {}

  /* implicit */ constexpr OptionSet(std::initializer_list<Derived> args) {
    for (Derived v : args) {
      value_ |= v;
    }
  }

  constexpr static Derived raw(Underlying raw) {
    Derived d;
    d.value_ = raw;
    return d;
  }

  explicit operator bool() const {
    return value_ != 0;
  }

  /**
   * Returns the raw bits.
   */
  Underlying asRaw() const {
    return value_;
  }

  /**
   * Returns true if every bit in `set` is set here too.
   *
   * Alias for `containsAllOf`.
   */
  bool contains(Derived set) const {
    return containsAllOf(set);
  }

  /**
   * Returns true if every bit in `set` is set here too.
   */
  bool containsAllOf(Derived set) const {
    return (value_ & set.value_) == set.value_;
  }

  /**
   * Returns true if any bit in `set` in set here too.
   */
  bool containsAnyOf(Derived set) const {
    return (value_ & set.value_) != 0;
  }

  /**
   * Returns true if all bits in `set` are unset here.
   */
  bool containsNoneOf(Derived set) const {
    return (value_ & set.value_) == 0;
  }

  /**
   * Returns true if no bits are set.
   */
  bool empty() const {
    return value_ == 0;
  }

  /**
   * Turns on the specified bits.
   */
  void set(Derived set) {
    value_ |= set.value_;
  }

  /**
   * Clears the specified bits.
   */
  void clear(Derived set) {
    value_ &= ~set.value_;
  }

  /**
   * Returns a space-delimited string representing the names of each set bit.
   * The name mapping is defined by `Derived::table`.
   */
  std::string format() const {
    // TODO: It might be nice to move the implementation of this function behind
    // a template on Raw to reduce the number of possible expansions. That would
    // require some sort of constexpr mapping from Derived onto Raw. Perhaps
    // that's as easy as asserting they have the same representation and casting
    // pointers.

    constexpr const NameTable& table = Derived::table;

    if (empty() || std::empty(table)) {
      return {};
    }

    // Precompute the length to avoid multiple allocations.
    size_t length = 0;
    for (auto [bit, name] : table) {
      // TODO: Should this assert bits are exact powers of two?
      if (containsAllOf(bit)) {
        if (length) {
          length++;
        }
        length += name.size();
      }
    }

    std::string result(length, 0);
    char* p = result.data();

    for (auto [bit, name] : table) {
      if (containsAllOf(bit)) {
        if (p != result.data()) {
          *p++ = ' ';
        }
        memcpy(p, name.data(), name.size());
        p += name.size();
      }
    }

    assert(static_cast<size_t>(p - result.data()) == length);

    return result;
  }

  Derived& operator|=(Derived that) {
    value_ |= that.value_;
    return static_cast<Derived&>(*this);
  }

  Derived& operator&=(Derived that) {
    value_ &= that.value_;
    return static_cast<Derived&>(*this);
  }

  /**
   * Returns the union of both sets.
   */
  friend Derived operator|(Derived lhs, Derived rhs) {
    return raw(lhs.value_ | rhs.value_);
  }

  /**
   * Returns the intersection of both sets.
   */
  friend Derived operator&(Derived lhs, Derived rhs) {
    return raw(lhs.value_ & rhs.value_);
  }

  friend bool operator==(Derived lhs, Derived rhs) {
    return lhs.value_ == rhs.value_;
  }

  friend bool operator!=(Derived lhs, Derived rhs) {
    return lhs.value_ != rhs.value_;
  }

 private:
  Underlying value_{};
};

} // namespace watchman
