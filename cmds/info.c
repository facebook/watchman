/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* version */
static void cmd_version(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

#ifdef WATCHMAN_BUILD_INFO
  set_prop(resp, "buildinfo", json_string(WATCHMAN_BUILD_INFO));
#endif

  send_and_dispose_response(client, resp);
}
W_CMD_REG("version", cmd_version, CMD_DAEMON|CMD_CLIENT, NULL)

/* get-sockname */
static void cmd_get_sockname(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  set_prop(resp, "sockname", json_string(get_sock_name()));

  send_and_dispose_response(client, resp);
}
W_CMD_REG("get-sockname", cmd_get_sockname, CMD_DAEMON|CMD_CLIENT, NULL)

/* get-pid */
static void cmd_get_pid(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  set_prop(resp, "pid", json_integer(getpid()));

  send_and_dispose_response(client, resp);
}
W_CMD_REG("get-pid", cmd_get_pid, CMD_DAEMON, NULL)

static void cmd_get_config(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  json_t *config;

  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments for 'get-config'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);

  if (!root) {
    return;
  }

  resp = make_response();

  w_root_lock(root);
  {
    config = root->config_file;
    if (config) {
      // set_prop will claim this ref
      json_incref(config);
    }
  }
  w_root_unlock(root);

  if (!config) {
    // set_prop will own this
    config = json_object();
  }

  json_incref(root->config_file);
  set_prop(resp, "config", config);
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("get-config", cmd_get_config, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
