/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/CommandRegistry.h"
#include <unordered_map>
#include <unordered_set>
#include "watchman/Errors.h"
#include "watchman/thirdparty/jansson/jansson.h"

namespace {
using namespace watchman;

struct reg {
  std::unordered_map<std::string, CommandDefinition*> commands;
  std::unordered_set<std::string> capabilities;

  reg() {
    commands.reserve(16);
    capabilities.reserve(128);
  }
};

// Meyers singleton to avoid SIOF problems
reg& get_reg() {
  static auto* s = new reg;
  return *s;
}
} // namespace

namespace watchman {

void register_command(CommandDefinition& def) {
  get_reg().commands.emplace(def.name, &def);
  capability_register(("cmd-" + std::string{def.name}).c_str());
}

CommandDefinition* lookup_command(
    std::string_view cmd_name,
    CommandFlags mode) {
  // TODO: Eliminate this copy in the lookup.
  auto it = get_reg().commands.find(std::string{cmd_name});
  auto def = it == get_reg().commands.end() ? nullptr : it->second;

  if (def) {
    if (mode && def->flags.containsNoneOf(mode)) {
      throw CommandValidationError(
          "command ", cmd_name, " not available in this mode");
    }
    return def;
  }

  if (mode) {
    throw CommandValidationError("unknown command ", cmd_name);
  }

  return nullptr;
}

std::vector<CommandDefinition*> get_all_commands() {
  std::vector<CommandDefinition*> defs;
  for (auto& it : get_reg().commands) {
    defs.emplace_back(it.second);
  }
  return defs;
}

void capability_register(const char* name) {
  get_reg().capabilities.emplace(name);
}

bool capability_supported(std::string_view name) {
  // TODO: Eliminate this copy.
  return get_reg().capabilities.find(std::string{name}) !=
      get_reg().capabilities.end();
}

json_ref capability_get_list() {
  auto& caps = get_reg().capabilities;

  auto arr = json_array_of_size(caps.size());
  for (auto& name : caps) {
    json_array_append(arr, typed_string_to_json(name.c_str()));
  }

  return arr;
}

} // namespace watchman
