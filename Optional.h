/* Copyright 2018-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <stdexcept>
#include <folly/Optional.h>

namespace watchman {

#if 1
using nullopt_t = folly::None;
static const auto nullopt = folly::none;
#else
/** placeholder type used to indicate that an optional
 * holds no value */
struct nullopt_t {};
/** placeholder value for initializing optionals empty */
static constexpr nullopt_t nullopt{};
#endif

#if 1
using BadOptionalAccess = folly::OptionalEmptyException;
#else
class BadOptionalAccess : public std::exception {
 public:
  const char* what() const noexcept override {
    return "bad optional access";
  }
};
#endif

#if 1
template <typename T>
using Optional = folly::Optional<T>;
#else
/** A lightweight stand-in for std::optional */
template <typename T>
class Optional {
  bool hasValue_{false};
  union {
    uint8_t dummy_;
    T value_;
  };

 public:
  Optional() {}

  /* implicit */ Optional(nullopt_t) {}

  void reset() noexcept {
    if (hasValue_) {
      value_.~T();
      hasValue_ = false;
    }
  }

  ~Optional() {
    reset();
  }

  /* implicit */ Optional(const Optional& other) {
    if (other.hasValue_) {
      new (&value_) T(other.value_);
      hasValue_ = true;
    }
  }

  /* implicit */ Optional(Optional&& other) {
    *this = std::move(other);
  }

  Optional& operator=(Optional&& other) noexcept {
    if (&other != this) {
      reset();

      if (other.hasValue_) {
        new (&value_) T(std::move(other.value_));
        hasValue_ = true;
        other.reset();
      }
    }
    return *this;
  }

  Optional& operator=(const Optional& other) noexcept {
    if (&other != this) {
      reset();

      if (other.hasValue_) {
        new (&value_) T(other.value_);
        hasValue_ = true;
      }
    }
    return *this;
  }

  /* implicit */ Optional(T other) : value_(std::move(other)) {
    // take care to assign this after the move has succeeded
    hasValue_ = true;
  }

  bool has_value() const noexcept {
    return hasValue_;
  }

  const T& value() const& {
    if (hasValue_) {
      return value_;
    }
    throw BadOptionalAccess();
  }

  T& value() & {
    if (hasValue_) {
      return value_;
    }
    throw BadOptionalAccess();
  }

  const T* operator->() const {
    return &value();
  }

  T* operator->() {
    return &value();
  }

  const T& operator*() const& {
    return value();
  }

  T& operator*() & {
    return value();
  }
};
#endif

} // namespace watchman
