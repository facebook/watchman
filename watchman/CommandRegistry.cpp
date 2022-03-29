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

namespace watchman {

namespace {

struct CommandRegistry {
  std::unordered_map<std::string, CommandDefinition*> commands;
  std::unordered_set<std::string> capabilities;

  CommandRegistry() {
    commands.reserve(16);
    capabilities.reserve(128);
  }

  static CommandRegistry& get() {
    // Meyers singleton to avoid SIOF problems
    static auto* s = new CommandRegistry;
    return *s;
  }
};

} // namespace

CommandDefinition* CommandDefinition::lookup(
    std::string_view name,
    CommandFlags mode) {
  auto& reg = CommandRegistry::get();

  // TODO: Eliminate this copy in the lookup.
  auto it = reg.commands.find(std::string{name});
  auto def = it == reg.commands.end() ? nullptr : it->second;

  if (def) {
    if (mode && def->flags.containsNoneOf(mode)) {
      throw CommandValidationError(
          "command ", name, " not available in this mode");
    }
    return def;
  }

  if (mode) {
    throw CommandValidationError("unknown command ", name);
  }

  return nullptr;
}

std::vector<CommandDefinition*> CommandDefinition::getAll() {
  std::vector<CommandDefinition*> defs;
  for (auto& it : CommandRegistry::get().commands) {
    defs.emplace_back(it.second);
  }
  return defs;
}

void CommandDefinition::register_() {
  CommandRegistry::get().commands.emplace(name, this);
  capability_register(("cmd-" + std::string{name}).c_str());
}

void capability_register(const char* name) {
  CommandRegistry::get().capabilities.emplace(name);
}

bool capability_supported(std::string_view name) {
  auto& reg = CommandRegistry::get();
  // TODO: Eliminate this copy.
  return reg.capabilities.find(std::string{name}) != reg.capabilities.end();
}

json_ref capability_get_list() {
  auto& caps = CommandRegistry::get().capabilities;

  auto arr = json_array_of_size(caps.size());
  for (auto& name : caps) {
    json_array_append(arr, typed_string_to_json(name.c_str()));
  }

  return arr;
}

} // namespace watchman
