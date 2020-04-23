/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <stdexcept>

namespace watchman {
using command_func = void (*)(watchman_client* client, const json_ref& args);

// Should throw an exception (ideally CommandValidationError) if validation
// fails
using cli_cmd_validate_func = void (*)(json_ref& args);

using command_flags = int;
constexpr int CMD_DAEMON = 1;
constexpr int CMD_CLIENT = 2;
constexpr int CMD_POISON_IMMUNE = 4;
constexpr int CMD_ALLOW_ANY_USER = 8;

struct command_handler_def {
  const char* name;
  command_func func;
  command_flags flags;
  cli_cmd_validate_func cli_validate;
};
} // namespace watchman

using watchman::CMD_ALLOW_ANY_USER;
using watchman::CMD_CLIENT;
using watchman::CMD_DAEMON;
using watchman::CMD_POISON_IMMUNE;

class CommandValidationError : public std::runtime_error {
 public:
  template <typename... Args>
  explicit CommandValidationError(Args&&... args)
      : std::runtime_error(folly::to<std::string>(
            "failed to validate command: ",
            std::forward<Args>(args)...)) {}
};

// For commands that take the root dir as the second parameter,
// realpath's that parameter on the client side and updates the
// argument list
void w_cmd_realpath_root(json_ref& args);

// Try to find a project root that contains the path `resolved`. If found,
// modify `resolved` to hold the path to the root project and return true.
// Else, return false.
// root_files should be derived from a call to cfg_compute_root_files, and it
// should not be null.  cfg_compute_root_files ensures that .watchmanconfig is
// first in the returned list of files.  This is important because it is the
// definitive indicator for the location of the project root.
bool find_project_root(
    const json_ref& root_files,
    w_string_piece& resolved,
    w_string_piece& relpath);

void preprocess_command(
    json_ref& args,
    enum w_pdu_type output_pdu,
    uint32_t output_capabilities);
bool dispatch_command(
    struct watchman_client* client,
    const json_ref& args,
    int mode);
bool try_client_mode_command(const json_ref& cmd, bool pretty);
void w_register_command(watchman::command_handler_def& defs);

/**
 * Provide a way to query (and eventually modify) command line arguments
 *
 * This is not thread-safe and should only be invoked from main()
 */
watchman::command_handler_def* lookup(const w_string& cmd_name, int mode);

#define W_CMD_REG_1(symbol, name, func, flags, clivalidate) \
  static w_ctor_fn_type(symbol) {                           \
    static ::watchman::command_handler_def d = {            \
        name, func, flags, clivalidate};                    \
    w_register_command(d);                                  \
  }                                                         \
  w_ctor_fn_reg(symbol)

#define W_CMD_REG(name, func, flags, clivalidate) \
  W_CMD_REG_1(w_gen_symbol(w_cmd_register_), name, func, flags, clivalidate)

#define W_CAP_REG1(symbol, name)  \
  static w_ctor_fn_type(symbol) { \
    w_capability_register(name);  \
  }                               \
  w_ctor_fn_reg(symbol)

#define W_CAP_REG(name) W_CAP_REG1(w_gen_symbol(w_cap_reg_), name)

void w_capability_register(const char* name);
bool w_capability_supported(const w_string& name);
json_ref w_capability_get_list(void);

void send_error_response(
    struct watchman_client* client,
    WATCHMAN_FMT_STRING(const char* fmt),
    ...) WATCHMAN_FMT_ATTR(2, 3);
void send_and_dispose_response(
    struct watchman_client* client,
    json_ref&& response);
bool enqueue_response(
    struct watchman_client* client,
    json_ref&& json,
    bool ping);

// Resolve the root. Failure will throw a RootResolveError exception
std::shared_ptr<w_root_t> resolveRoot(
    struct watchman_client* client,
    const json_ref& args);

// Resolve the root, or if not found and the configuration permits,
// attempt to create it. throws RootResolveError on failure.
std::shared_ptr<w_root_t> resolveOrCreateRoot(
    struct watchman_client* client,
    const json_ref& args);

json_ref make_response(void);
void annotate_with_clock(const std::shared_ptr<w_root_t>& root, json_ref& resp);
void add_root_warnings_to_response(
    json_ref& response,
    const std::shared_ptr<w_root_t>& root);

bool clock_id_string(
    uint32_t root_number,
    uint32_t ticks,
    char* buf,
    size_t bufsize);

/* vim:ts=2:sw=2:et:
 */
