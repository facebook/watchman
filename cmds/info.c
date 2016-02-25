/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool query_caps(json_t *response, json_t *result,
    json_t *arr, bool required) {
  size_t i;
  bool have_all = true;

  for (i = 0; i < json_array_size(arr); i++) {
    json_t *ele = json_array_get(arr, i);
    const char *capname = json_string_value(ele);
    bool have = capname && w_capability_supported(json_string_value(ele));
    if (!have) {
      have_all = false;
    }
    if (!capname) {
      break;
    }
    set_prop(result, capname, json_boolean(have));
    if (required && !have) {
      char *buf = NULL;
      ignore_result(asprintf(&buf,
            "client required capability `%s` is not supported by this server",
            capname));
      set_prop(response, "error", json_string_nocheck(buf));
      w_log(W_LOG_ERR, "version: %s\n", buf);
      free(buf);

      // Only trigger the error on the first one we hit.  Ideally
      // we'd tell the user about all of them, but it is a PITA to
      // join and print them here in C :-/
      required = false;
    }
  }
  return have_all;
}

/* version */
static void cmd_version(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

#ifdef WATCHMAN_BUILD_INFO
  set_prop(resp, "buildinfo", json_string(WATCHMAN_BUILD_INFO));
#endif

  /* ["version"]
   *    -> just returns the basic version information.
   * ["version", {"required": ["foo"], "optional": ["bar"]}]
   *    -> includes capability matching information
   */

  if (json_array_size(args) == 2) {
    const char *ignored;
    json_t *req_cap = NULL;
    json_t *opt_cap = NULL;
    json_t *cap_res;

    json_unpack(args, "[s, {s?:o, s?:o}]",
        &ignored,
        "required", &req_cap,
        "optional", &opt_cap);

    cap_res = json_object_of_size(
        (opt_cap ? json_array_size(opt_cap) : 0) +
        (req_cap ? json_array_size(req_cap) : 0));

    if (opt_cap && json_is_array(opt_cap)) {
      query_caps(resp, cap_res, opt_cap, false);
    }
    if (req_cap && json_is_array(req_cap)) {
      query_caps(resp, cap_res, req_cap, true);
    }

    set_prop(resp, "capabilities", cap_res);
  }

  send_and_dispose_response(client, resp);
}
W_CMD_REG("version", cmd_version, CMD_DAEMON|CMD_CLIENT, NULL)

/* list-capabilities */
static void cmd_list_capabilities(struct watchman_client *client,
    json_t *args) {
  json_t *resp = make_response();
  unused_parameter(args);

  set_prop(resp, "capabilities", w_capability_get_list());
  send_and_dispose_response(client, resp);
}
W_CMD_REG("list-capabilities", cmd_list_capabilities,
    CMD_DAEMON|CMD_CLIENT, NULL)

/* get-sockname */
static void cmd_get_sockname(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  set_prop(resp, "sockname", json_string(get_sock_name()));

  send_and_dispose_response(client, resp);
}
W_CMD_REG("get-sockname", cmd_get_sockname, CMD_DAEMON|CMD_CLIENT, NULL)

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

  w_root_lock(root, "cmd_get_config");
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
