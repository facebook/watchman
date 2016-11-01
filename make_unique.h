/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <memory>

namespace watchman {

// Roughly equivalent to the C++14 std::make_unique
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T, typename Deleter, typename... Args>
std::unique_ptr<T, Deleter> make_unique(Args&&... args) {
  return std::unique_ptr<T, Deleter>(new T(std::forward<Args>(args)...));
}
}
