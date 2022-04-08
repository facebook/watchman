/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Command.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/watchman_stream.h"

namespace watchman {

Command Command::parse(const json_ref& pdu) {
  if (!json_array_size(pdu)) {
    throw CommandValidationError(
        "invalid command (expected an array with some elements!)");
  }

  const auto jstr = json_array_get(pdu, 0);
  const char* cmd_name = json_string_value(jstr);
  if (!cmd_name) {
    throw CommandValidationError(
        "invalid command: expected element 0 to be the command name");
  }

  const auto& pdu_array = pdu.array();
  auto args = json_array();
  auto& args_array = args.array();
  args_array.reserve(pdu_array.size() - 1);
  for (size_t i = 1; i < pdu_array.size(); ++i) {
    args_array.push_back(pdu_array[i]);
  }

  return Command{w_string{cmd_name}, std::move(args)};
}

json_ref Command::render() const {
  auto result = json_array();
  auto& arr = result.array();
  arr.push_back(w_string_to_json(name_));
  arr.insert(arr.end(), args_.array().begin(), args_.array().end());
  return result;
}

void Command::validateOrExit(PduType output_pdu, uint32_t output_capabilities) {
  auto* def = CommandDefinition::lookup(name_.view(), CommandFlags{});
  if (!def) {
    // Nothing known about it, pass the command on anyway for forwards
    // compatibility
    return;
  }

  if (!def->validator) {
    return;
  }

  try {
    def->validator(*this);
  } catch (const std::exception& exc) {
    auto err = json_object(
        {{"error", typed_string_to_json(exc.what(), W_STRING_MIXED)},
         {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)},
         {"cli_validated", json_true()}});

    PduBuffer jr;
    jr.pduEncodeToStream(output_pdu, output_capabilities, err, w_stm_stdout());
    exit(1);
  }
}

} // namespace watchman
