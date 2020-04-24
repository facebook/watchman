/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <unordered_map>
#include <unordered_set>

#include "CommandRegistry.h"
#include "thirdparty/jansson/jansson.h"

using folly::StringPiece;

namespace {
using namespace watchman;

struct reg {
  std::unordered_map<std::string, command_handler_def*> commands;
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

void register_command(command_handler_def& def) {
  get_reg().commands.emplace(def.name, &def);

  char capname[128];
  snprintf(capname, sizeof(capname), "cmd-%s", def.name);
  capability_register(capname);
}

command_handler_def* lookup_command(folly::StringPiece cmd_name, int mode) {
  auto it = get_reg().commands.find(cmd_name.str());
  auto def = it == get_reg().commands.end() ? nullptr : it->second;

  if (def) {
    if (mode && ((def->flags & mode) == 0)) {
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

std::vector<command_handler_def*> get_all_commands() {
  std::vector<command_handler_def*> defs;
  for (auto& it : get_reg().commands) {
    defs.emplace_back(it.second);
  }
  return defs;
}

void capability_register(const char* name) {
  get_reg().capabilities.emplace(name);
}

bool capability_supported(folly::StringPiece name) {
  return get_reg().capabilities.find(name.str()) !=
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
