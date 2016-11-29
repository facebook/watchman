/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#ifdef _WIN32
#include "Win32Handle.h"
#include "watchman.h"

namespace watchman {

Win32Handle::~Win32Handle() {
  close();
}

Win32Handle::Win32Handle(intptr_t h) : h_(h) {
  // Normalize to a single invalid value for validity checks
  if (h_ == intptr_t(INVALID_HANDLE_VALUE)) {
    h_ = 0;
  }
}

Win32Handle::Win32Handle(Win32Handle&& other) noexcept : h_(other.release()) {}

Win32Handle& Win32Handle::operator=(Win32Handle&& other) noexcept {
  close();
  h_ = other.h_;
  other.h_ = 0;
  return *this;
}

void Win32Handle::close() {
  if (h_) {
    CloseHandle((HANDLE)h_);
    h_ = 0;
  }
}

intptr_t Win32Handle::release() {
  intptr_t res = h_;
  h_ = 0;
  return res;
}
}
#endif // _WIN32
