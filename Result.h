/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <exception>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace watchman {

// To avoid some horrible special casing for the void type in template
// metaprogramming we use Unit to denote an uninteresting value type.
struct Unit {
  // Lift void -> Unit if T is void, else T
  template <typename T>
  struct Lift : std::conditional<std::is_same<T, void>::value, Unit, T> {};
};

// Represents the Result of an operation, and thus can hold either
// a value or an error, or neither.  This is similar to the folly::Try
// type and also to the rust Result type.  The contained Error type
// can be replaced by an arbitrary error container as a stronger nod
// toward the rust Result type and is useful in situations where
// throwing and catching exceptions is undesirable.
template <typename Value, typename Error = std::exception_ptr>
class Result {
  static_assert(
      !std::is_reference<Value>::value && !std::is_reference<Error>::value,
      "Result may not be used with reference types");
  static_assert(
      !std::is_same<Value, Error>::value,
      "Value and Error must not be the same type");

  enum class State { kEMPTY, kVALUE, kERROR };

 public:
  using value_type = Value;
  using error_type = Error;

  // Default construct an empty Result
  Result() : state_(State::kEMPTY) {}

  ~Result() {
    // value_/error_ is a union, thus manual call of destructors
    switch (state_) {
      case State::kEMPTY:
        break;
      case State::kVALUE:
        value_.~Value();
        break;
      case State::kERROR:
        error_.~Error();
        break;
    }
  }

  // Copy a value into the result
  explicit Result(const Value& other) : state_(State::kVALUE), value_(other) {}

  // Move in value
  explicit Result(Value&& other)
      : state_(State::kVALUE), value_(std::move(other)) {}

  // Copy in error
  explicit Result(const Error& error) : state_(State::kERROR), error_(error) {}

  // Move in error
  explicit Result(Error&& error)
      : state_(State::kERROR), error_(std::move(error)) {}

  // Move construct
  explicit Result(Result&& other) noexcept : state_(other.state_) {
    switch (state_) {
      case State::kEMPTY:
        break;
      case State::kVALUE:
        new (&value_) Value(std::move(other.value_));
        break;
      case State::kERROR:
        new (&error_) Error(std::move(other.error_));
        break;
    }
    other.~Result();
    other.state_ = State::kEMPTY;
  }

  // Move assign
  Result& operator=(Result&& other) noexcept {
    if (&other != this) {
      this->~Result();

      state_ = other.state_;
      switch (state_) {
        case State::kEMPTY:
          break;
        case State::kVALUE:
          new (&value_) Value(std::move(other.value_));
          break;
        case State::kERROR:
          new (&error_) Error(std::move(other.error_));
          break;
      }

      other.~Result();
      other.state_ = State::kEMPTY;
    }
    return *this;
  }

  // Copy construct
  Result(const Result& other) {
    static_assert(
        std::is_copy_constructible<Value>::value &&
            std::is_copy_constructible<Error>::value,
        "Value and Error must be copyable for "
        "Result<Value,Error> to be copyable");

    state_ = other.state_;
    switch (state_) {
      case State::kEMPTY:
        break;
      case State::kVALUE:
        new (&value_) Value(other.value_);
        break;
      case State::kERROR:
        new (&error_) Error(other.error_);
        break;
    }
  }

  // Copy assign
  Result& operator=(const Result& other) {
    static_assert(
        std::is_copy_constructible<Value>::value &&
            std::is_copy_constructible<Error>::value,
        "Value and Error must be copyable for "
        "Result<Value,Error> to be copyable");

    if (&other != this) {
      this->~Result();
      state_ = other.state_;
      switch (state_) {
        case State::kEMPTY:
          break;
        case State::kVALUE:
          new (&value_) Value(other.value_);
          break;
        case State::kERROR:
          new (&error_) Error(other.error_);
          break;
      }
    }
    return *this;
  }

  bool hasValue() const {
    return state_ == State::kVALUE;
  }

  bool hasError() const {
    return state_ == State::kERROR;
  }

  bool empty() const {
    return state_ == State::kEMPTY;
  }

  // If Result does not contain a valid Value, throw
  // the Error value.  If there is no error value,
  // throw a logic error.
  // This variant is used when Error is std::exception_ptr.
  template <typename E = Error>
  typename std::enable_if<std::is_same<E, std::exception_ptr>::value>::type
  throwIfError() const {
    switch (state_) {
      case State::kVALUE:
        return;
      case State::kEMPTY:
        throw std::logic_error("Uninitialized Result");
      case State::kERROR:
        std::rethrow_exception(error_);
    }
  }

  // If Result does not contain a valid Value, throw a logic error.
  // This variant is used when Error is std::error_code.
  template <typename E = Error>
  typename std::enable_if<std::is_same<E, std::error_code>::value>::type
  throwIfError() const {
    switch (state_) {
      case State::kVALUE:
        return;
      case State::kEMPTY:
        throw std::logic_error("Uninitialized Result");
      case State::kERROR:
        throw std::system_error(error_);
    }
  }

  // If Result does not contain a valid Value, throw a logic error.
  // This variant is used when Error is not std::exception_ptr or
  // std::error_code.
  template <typename E = Error>
  typename std::enable_if<
      !std::is_same<E, std::exception_ptr>::value &&
      !std::is_same<E, std::error_code>::value>::type
  throwIfError() const {
    switch (state_) {
      case State::kVALUE:
        return;
      case State::kEMPTY:
        throw std::logic_error("Uninitialized Result");
      case State::kERROR:
        throw std::logic_error("Result holds Error, not Value");
    }
  }

  // Get a mutable reference to the value.  If the value is
  // not assigned, an exception will be thrown by throwIfError().
  Value& value() & {
    throwIfError();
    return value_;
  }

  // Get an rvalue reference to the value.  If the value is
  // not assigned, an exception will be thrown by throwIfError().
  Value&& value() && {
    throwIfError();
    return value_;
  }

  // Get a const reference to the value.  If the value is
  // not assigned, an exception will be thrown by throwIfError().
  const Value& value() const& {
    throwIfError();
    return value_;
  }

  // Throws a logic exception if the result does not contain an Error
  void throwIfNotError() {
    switch (state_) {
      case State::kVALUE:
        throw std::logic_error("Result holds Value, not Error");
      case State::kEMPTY:
        throw std::logic_error("Uninitialized Result");
      case State::kERROR:
        return;
    }
  }

  // Get a mutable reference to the error.  If the error is
  // not assigned, an exception will be thrown by throwIfNotError().
  Error& error() & {
    throwIfNotError();
    return error_;
  }

  // Get an rvalue reference to the error.  If the error is
  // not assigned, an exception will be thrown by throwIfNotError().
  Error&& error() && {
    throwIfNotError();
    return error_;
  }

  // Get a const reference to the error.  If the error is
  // not assigned, an exception will be thrown by throwIfNotError().
  const Error& error() const& {
    throwIfNotError();
    return error_;
  }

 private:
  State state_;
  union {
    Value value_;
    Error error_;
  };
};

// Helper for making a Result from a value; auto-deduces the Value type.
// The Error type can be overridden and is listed first because the whole
// point of this is to avoid specifying the Value type.
template <typename Error = std::exception_ptr, typename T>
Result<typename std::decay<T>::type, Error> makeResult(T&& t) {
  return Result<typename std::decay<T>::type, Error>(std::forward<T>(t));
}

// Helper for populating a Result with the return value from a lambda.
// If the lambda throws an exception it will be captured into the Result.
// This is the non-void return type flavor.
template <typename Func>
typename std::enable_if<
    !std::is_same<typename std::result_of<Func()>::type, void>::value,
    Result<typename std::result_of<Func()>::type>>::type
makeResultWith(Func&& func) {
  using ResType = typename std::result_of<Func()>::type;

  try {
    return Result<ResType>(func());
  } catch (const std::exception& e) {
    return Result<ResType>(std::current_exception());
  }
}

// Helper for populating a Result with the return value from a lambda.
// If the lambda throws an exception it will be captured into the Result.
// This is the void return type flavor; it produces Result<Unit>
template <typename Func>
typename std::enable_if<
    std::is_same<typename std::result_of<Func()>::type, void>::value,
    Result<Unit>>::type
makeResultWith(Func&& func) {
  try {
    func();
    return Result<Unit>(Unit{});
  } catch (const std::exception& e) {
    return Result<Unit>(std::current_exception());
  }
}
}
