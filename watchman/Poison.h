/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <folly/Synchronized.h>
#include <chrono>
#include <string>

namespace watchman {

/**
 * Some error conditions will put us into a non-recoverable state where we
 * can't guarantee that we will be operating correctly.  Rather than suffering
 * in silence and misleading our clients, we'll poison ourselves and advertise
 * that we have done so and provide some advice on how the user can cure us.
 */
extern folly::Synchronized<std::string> poisoned_reason;

void set_poison_state(
    const w_string& dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err);

} // namespace watchman
