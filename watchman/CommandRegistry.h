/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdexcept>
#include <vector>

#include "watchman/OptionSet.h"
#include "watchman/watchman_preprocessor.h"

class json_ref;

namespace watchman {

class Client;
class Command;

using CommandHandler = void (*)(Client* client, const json_ref& args);

// Should throw an exception (ideally CommandValidationError) if validation
// fails
using CommandValidator = void (*)(Command& command);

struct CommandFlags : OptionSet<CommandFlags, uint8_t> {};

inline constexpr auto CMD_DAEMON = CommandFlags::raw(1);
inline constexpr auto CMD_CLIENT = CommandFlags::raw(2);
inline constexpr auto CMD_POISON_IMMUNE = CommandFlags::raw(4);
inline constexpr auto CMD_ALLOW_ANY_USER = CommandFlags::raw(8);

struct CommandDefinition {
  std::string_view name;
  CommandHandler handler;
  CommandFlags flags;
  CommandValidator validator;
};

void register_command(CommandDefinition& defs);

/**
 * Provide a way to query (and eventually modify) command line arguments
 *
 * This is not thread-safe and should only be invoked from main()
 */
CommandDefinition* lookup_command(std::string_view cmd_name, CommandFlags mode);

std::vector<CommandDefinition*> get_all_commands();

#define W_CMD_REG_1(symbol, name, func, flags, clivalidate)                    \
  static w_ctor_fn_type(symbol) {                                              \
    static ::watchman::CommandDefinition d = {name, func, flags, clivalidate}; \
    ::watchman::register_command(d);                                           \
  }                                                                            \
  w_ctor_fn_reg(symbol)

#define W_CMD_REG(name, func, flags, clivalidate) \
  W_CMD_REG_1(w_gen_symbol(w_cmd_register_), name, func, flags, clivalidate)

#define W_CAP_REG1(symbol, name)           \
  static w_ctor_fn_type(symbol) {          \
    ::watchman::capability_register(name); \
  }                                        \
  w_ctor_fn_reg(symbol)

#define W_CAP_REG(name) W_CAP_REG1(w_gen_symbol(w_cap_reg_), name)

void capability_register(const char* name);
bool capability_supported(std::string_view name);
json_ref capability_get_list();

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
