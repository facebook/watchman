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
    CommandDefinition* def = lookup_command(command.name(), CommandFlags{});

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

bool dispatch_command(
    Client* client,
    const Command& command,
    CommandFlags mode) {
  // Stash a reference to the current command to make it easier to log
  // the command context in some of the error paths
  client->current_command = &command;
  SCOPE_EXIT {
    client->current_command = nullptr;
  };

  try {
    CommandDefinition* def = lookup_command(command.name(), mode);
    if (!def) {
      client->sendErrorResponse("Unknown command");
      return false;
    }

    if (!poisoned_reason.rlock()->empty() &&
        !def->flags.contains(CMD_POISON_IMMUNE)) {
      client->sendErrorResponse(*poisoned_reason.rlock());
      return false;
    }

    if (!client->client_is_owner && !def->flags.contains(CMD_ALLOW_ANY_USER)) {
      client->sendErrorResponse(
          "you must be the process owner to execute '{}'", def->name);
      return false;
    }

    // Scope for the perf sample
    {
      logf(DBG, "dispatch_command: {}\n", def->name);
      auto sample_name = "dispatch_command:" + std::string{def->name};
      PerfSample sample(sample_name.c_str());
      client->perf_sample = &sample;
      SCOPE_EXIT {
        client->perf_sample = nullptr;
      };

      sample.set_wall_time_thresh(
          cfg_get_double("slow_command_log_threshold_seconds", 1.0));

      // TODO: It's silly to convert a Command back into JSON after parsing it.
      // Let's change `func` to take a Command after Command knows what a root
      // path is.
      auto rendered = command.render();

      def->handler(client, rendered);

      if (sample.finish()) {
        sample.add_meta("args", std::move(rendered));
        sample.add_meta(
            "client",
            json_object(
                {{"pid", json_integer(client->stm->getPeerProcessID())}}));
        sample.log();
      }
      logf(DBG, "dispatch_command: {} (completed)\n", def->name);
    }

    return true;
  } catch (const std::exception& e) {
    client->sendErrorResponse(folly::exceptionStr(e));
    return false;
  }
}

/* vim:ts=2:sw=2:et:
 */
