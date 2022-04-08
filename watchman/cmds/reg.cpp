/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/ExceptionString.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include "watchman/Client.h"
#include "watchman/Command.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/Logging.h"
#include "watchman/PDU.h"
#include "watchman/Poison.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_stream.h"

using namespace watchman;

void preprocess_command(
    Command& command,
    PduType output_pdu,
    uint32_t output_capabilities) {
  try {
    auto* def = CommandDefinition::lookup(command.name(), CommandFlags{});
    if (!def) {
      // Nothing known about it, pass the command on anyway for forwards
      // compatibility
      return;
    }

    if (def->validator) {
      def->validator(command);
    }
  } catch (const std::exception& exc) {
    PduBuffer jr;

    auto err = json_object(
        {{"error", typed_string_to_json(exc.what(), W_STRING_MIXED)},
         {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)},
         {"cli_validated", json_true()}});

    jr.pduEncodeToStream(output_pdu, output_capabilities, err, w_stm_stdout());
    exit(1);
  }
}

/* vim:ts=2:sw=2:et:
 */
