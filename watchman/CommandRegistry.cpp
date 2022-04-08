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

CommandDefinition* commandsList = nullptr;

struct CommandRegistry {
  std::unordered_set<std::string> capabilities;

  CommandRegistry() {
    capabilities.reserve(128);
  }

  static CommandRegistry& get() {
    // Meyers singleton to avoid SIOF problems
    static auto* s = new CommandRegistry;
    return *s;
  }
};

} // namespace

CommandDefinition::CommandDefinition(
    std::string_view name,
    std::string_view capname,
    CommandHandler handler,
    CommandFlags flags,
    CommandValidator validator)
    : name{name}, handler{handler}, flags{flags}, validator{validator} {
  next_ = commandsList;
  commandsList = this;

  capability_register(capname);
}

const CommandDefinition* CommandDefinition::lookup(
    std::string_view name,
    CommandFlags mode) {
  // You can imagine optimizing this into a sublinear lookup but the command
  // list is small and constant.
  for (const auto* def = commandsList; def; def = def->next_) {
    if (name == def->name) {
      if (mode && def->flags.containsNoneOf(mode)) {
        throw CommandValidationError(
            "command ", name, " not available in this mode");
      }
      return def;
    }
  }

  if (mode) {
    throw CommandValidationError("unknown command ", name);
  } else {
    return nullptr;
  }
}

std::vector<const CommandDefinition*> CommandDefinition::getAll() {
  size_t n = 0;
  for (const auto* p = commandsList; p; p = p->next_) {
    ++n;
  }

  std::vector<const CommandDefinition*> defs;
  defs.reserve(n);
  for (auto* p = commandsList; p; p = p->next_) {
    defs.push_back(p);
  }
  return defs;
}

void capability_register(std::string_view name) {
  CommandRegistry::get().capabilities.emplace(std::string{name});
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
