/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_CMD_H
#define WATCHMAN_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*watchman_command_func)(
    struct watchman_client *client,
    json_t *args);

typedef bool (*watchman_cli_cmd_validate_func)(
    json_t *args, char **errmsg);

#define CMD_DAEMON 1
#define CMD_CLIENT 2
#define CMD_POISON_IMMUNE 4
#define CMD_ALLOW_ANY_USER 8
struct watchman_command_handler_def {
  const char *name;
  watchman_command_func func;
  int flags;
  watchman_cli_cmd_validate_func cli_validate;
};

// For commands that take the root dir as the second parameter,
// realpath's that parameter on the client side and updates the
// argument list
bool w_cmd_realpath_root(json_t *args, char **errmsg);

void preprocess_command(json_t *args, enum w_pdu_type output_pdu,
                        uint32_t output_capabilities);
bool dispatch_command(struct watchman_client *client, json_t *args, int mode);
bool try_client_mode_command(json_t *cmd, bool pretty);
void w_register_command(struct watchman_command_handler_def *defs);

#define W_CMD_REG_1(symbol, name, func, flags, clivalidate) \
  static w_ctor_fn_type(symbol) {                    \
    static struct watchman_command_handler_def d = { \
      name, func, flags, clivalidate                 \
    };                                               \
    w_register_command(&d);                          \
  }                                                  \
  w_ctor_fn_reg(symbol)

#define W_CMD_REG(name, func, flags, clivalidate) \
  W_CMD_REG_1(w_gen_symbol(w_cmd_register_), \
      name, func, flags, clivalidate)

#define W_CAP_REG1(symbol, name)  \
  static w_ctor_fn_type(symbol) { \
    w_capability_register(name);  \
  }                               \
  w_ctor_fn_reg(symbol)

#define W_CAP_REG(name) \
  W_CAP_REG1(w_gen_symbol(w_cap_reg_), name)

void w_capability_register(const char *name);
bool w_capability_supported(const w_string_t *name);
json_t *w_capability_get_list(void);

void send_error_response(struct watchman_client *client,
    const char *fmt, ...);
void send_and_dispose_response(struct watchman_client *client,
    json_t *response);
bool enqueue_response(struct watchman_client *client,
    json_t *json, bool ping);

w_root_t *resolve_root_or_err(
    struct watchman_client *client,
    json_t *args,
    int root_index,
    bool create);

json_t *make_response(void);
void annotate_with_clock(w_root_t *root, json_t *resp);
void add_root_warnings_to_response(json_t *response, w_root_t *root);

bool clock_id_string(uint32_t root_number, uint32_t ticks, char *buf,
    size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
