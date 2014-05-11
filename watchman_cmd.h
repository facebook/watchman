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

#define CMD_DAEMON 1
#define CMD_CLIENT 2
struct watchman_command_handler_def {
  const char *name;
  watchman_command_func func;
  int flags;
};

bool dispatch_command(struct watchman_client *client, json_t *args, int mode);
bool try_client_mode_command(json_t *cmd, bool pretty);
void w_register_command(struct watchman_command_handler_def *defs);
#define W_CMD_REG(name, func, flags) \
  static __attribute__((constructor)) \
  void w_gen_symbol(w_cmd_register_)(void) { \
    static struct watchman_command_handler_def d = { \
      name, func, flags                              \
    };                                               \
    w_register_command(&d);                          \
  }


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

bool clock_id_string(uint32_t root_number, uint32_t ticks, char *buf,
    size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
