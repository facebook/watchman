/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "watchman/thirdparty/jansson/jansson.h"

namespace watchman {

class Command {
 public:
  /**
   * Constructs a null command used only to start the Watchman server.
   */
  /* implicit */ Command(std::nullptr_t) {}

  Command(w_string name, json_ref args)
      : name_{std::move(name)}, args_{std::move(args)} {}

  /**
   * Parses a command from arbitrary JSON.
   *
   * Throws CommandValidationError if the JSON is invalid.
   */
  static Command parse(const json_ref& pdu);

  /**
   * Renders into a JSON (or BSER) PDU.
   */
  json_ref render() const;

  /**
   * The null command is used solely to start the server, and never actually
   * executed.
   */
  bool isNullCommand() const {
    return name_ == nullptr;
  }

  std::string_view name() const {
    return name_.view();
  }

  json_ref& args() {
    return args_;
  }

  const json_ref& args() const {
    return args_;
  }

 private:
  w_string name_;
  json_ref args_;
};

} // namespace watchman
