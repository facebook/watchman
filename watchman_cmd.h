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

struct watchman_command_handler_def {
  const char *name;
  watchman_command_func func;
};

bool dispatch_command(struct watchman_client *client, json_t *args);
bool try_client_mode_command(json_t *cmd, bool pretty);
void register_commands(struct watchman_command_handler_def *defs);

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

bool clock_id_string(uint32_t ticks, char *buf, size_t bufsize);

bool parse_watch_params(int start, json_t *args,
    struct watchman_rule **head_ptr,
    uint32_t *next_arg,
    char *errbuf, int errbuflen);


void cmd_find(struct watchman_client *client, json_t *args);
void cmd_loglevel(struct watchman_client *client, json_t *args);
void cmd_log(struct watchman_client *client, json_t *args);
void cmd_since(struct watchman_client *client, json_t *args);
void cmd_trigger_list(struct watchman_client *client, json_t *args);
void cmd_trigger_delete(struct watchman_client *client, json_t *args);
void cmd_trigger(struct watchman_client *client, json_t *args);
void cmd_watch(struct watchman_client *client, json_t *args);
void cmd_watch_list(struct watchman_client *client, json_t *args);
void cmd_watch_delete(struct watchman_client *client, json_t *args);
void cmd_query(struct watchman_client *client, json_t *args);
void cmd_subscribe(struct watchman_client *client, json_t *args);
void cmd_unsubscribe(struct watchman_client *client, json_t *args);
void cmd_version(struct watchman_client *client, json_t *args);
void cmd_clock(struct watchman_client *client, json_t *args);
void cmd_get_sockname(struct watchman_client *client, json_t *args);
void cmd_get_pid(struct watchman_client *client, json_t *args);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

