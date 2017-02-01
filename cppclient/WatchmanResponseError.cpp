/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "WatchmanConnection.h"

namespace watchman {

using namespace folly;

static const dynamic kError("error");

WatchmanResponseError::WatchmanResponseError(const folly::dynamic& response)
    : WatchmanError(response[kError].c_str()), response_(response) {}

const folly::dynamic& WatchmanResponseError::getResponse() const {
  return response_;
}
} // namespace watchman
