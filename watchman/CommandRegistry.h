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

/**
 * Validates a command's arguments. Runs on the client. May modify the given
 * command. Should throw an exception (ideally CommandValidationError) if
 * validation fails.
 */
using CommandValidator = void (*)(Command& command);

/**
 * Executes a command's primary action. Usually runs on the server, but there
 * are client-only commands.
 */
using CommandHandler = json_ref (*)(Client* client, const json_ref& args);

/**
 * For commands that support pretty, human-readable output, this function is
 * called, on the client, with a result PDU. It should print its output to
 * stdout.
 *
 * Only called when the output is a tty.
 */
using ResultPrinter = void (*)(const json_ref& result);

struct CommandFlags : OptionSet<CommandFlags, uint8_t> {};

inline constexpr auto CMD_DAEMON = CommandFlags::raw(1);
inline constexpr auto CMD_CLIENT = CommandFlags::raw(2);
inline constexpr auto CMD_POISON_IMMUNE = CommandFlags::raw(4);
inline constexpr auto CMD_ALLOW_ANY_USER = CommandFlags::raw(8);

struct CommandDefinition {
  const std::string_view name;
  const CommandFlags flags;
  const CommandValidator validator;
  const CommandHandler handler;
  const ResultPrinter result_printer;

  CommandDefinition(
      std::string_view name,
      std::string_view capname,
      CommandHandler handler,
      CommandFlags flags,
      CommandValidator validator,
      ResultPrinter result_printer);

  /**
   * Provide a way to query (and eventually modify) command line arguments
   *
   * This is not thread-safe and should only be invoked from main()
   */
  static const CommandDefinition* lookup(std::string_view name);

  static std::vector<const CommandDefinition*> getAll();

 private:
  // registration linkage
  CommandDefinition* next_ = nullptr;
};

/**
 * Provides a typed interface for CommandDefinition that can optionally handle
 * validation, result-printing, request decoding, and response encoding.
 */
template <typename T>
class TypedCommand : public CommandDefinition {
 public:
  /// Override to implement a validator.
  static constexpr CommandValidator validate = nullptr;

  /// Override to implement a result printer.
  static constexpr ResultPrinter printResult = nullptr;

  TypedCommand()
      : CommandDefinition{
            T::name,
            // TODO: eliminate this allocation
            std::string{"cmd-"} + std::string{T::name},
            T::handle,
            T::flags,
            T::validate,
            T::printResult} {}
};

static_assert(
    std::is_trivially_destructible_v<CommandDefinition>,
    "CommandDefinition should remain unchanged until process exit");

void capability_register(std::string_view name);
bool capability_supported(std::string_view name);
json_ref capability_get_list();

#define W_CMD_REG(name, func, flags, clivalidate)                       \
  static const ::watchman::CommandDefinition w_gen_symbol(w_cmd_def_) { \
    (name), "cmd-" name, (func), (flags), (clivalidate), nullptr        \
  }

#define WATCHMAN_COMMAND(name, class_) static class_ reg_##name

#define W_CAP_REG1(symbol, name)           \
  static w_ctor_fn_type(symbol) {          \
    ::watchman::capability_register(name); \
  }                                        \
  w_ctor_fn_reg(symbol)

#define W_CAP_REG(name) W_CAP_REG1(w_gen_symbol(w_cap_reg_), name)

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
