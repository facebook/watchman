/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool query_caps(
    json_ref& response,
    json_ref& result,
    const json_ref& arr,
    bool required) {
  size_t i;
  bool have_all = true;

  for (i = 0; i < json_array_size(arr); i++) {
    const auto& ele = arr.at(i);
    const char* capname = json_string_value(ele);
    bool have = w_capability_supported(json_to_w_string(ele));
    if (!have) {
      have_all = false;
    }
    if (!capname) {
      break;
    }
    result.set(capname, json_boolean(have));
    if (required && !have) {
      char *buf = NULL;
      ignore_result(asprintf(
          &buf,
          "client required capability `%s` is not supported by this server",
          capname));
      response.set("error", typed_string_to_json(buf, W_STRING_UNICODE));
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
static void cmd_version(struct watchman_client* client, const json_ref& args) {
  auto resp = make_response();

#ifdef WATCHMAN_BUILD_INFO
  resp.set(
      "buildinfo", typed_string_to_json(WATCHMAN_BUILD_INFO, W_STRING_UNICODE));
#endif

  /* ["version"]
   *    -> just returns the basic version information.
   * ["version", {"required": ["foo"], "optional": ["bar"]}]
   *    -> includes capability matching information
   */

  if (json_array_size(args) == 2) {
    const auto& arg_obj = args.at(1);

    auto req_cap = arg_obj.get_default("required");
    auto opt_cap = arg_obj.get_default("optional");

    auto cap_res = json_object_of_size(
        (opt_cap ? json_array_size(opt_cap) : 0) +
        (req_cap ? json_array_size(req_cap) : 0));

    if (opt_cap && json_is_array(opt_cap)) {
      query_caps(resp, cap_res, opt_cap, false);
    }
    if (req_cap && json_is_array(req_cap)) {
      query_caps(resp, cap_res, req_cap, true);
    }

    resp.set("capabilities", std::move(cap_res));
  }

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("version", cmd_version, CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
          NULL)

/* list-capabilities */
static void cmd_list_capabilities(
    struct watchman_client* client,
    const json_ref&) {
  auto resp = make_response();

  resp.set("capabilities", w_capability_get_list());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("list-capabilities", cmd_list_capabilities,
          CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER, NULL)

/* get-sockname */
static void cmd_get_sockname(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();

  resp.set("sockname", typed_string_to_json(get_sock_name(), W_STRING_BYTE));

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("get-sockname", cmd_get_sockname,
          CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER, NULL)

static void cmd_get_config(
    struct watchman_client* client,
    const json_ref& args) {
  json_ref config;

  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments for 'get-config'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto resp = make_response();

  config = root->config_file;

  if (!config) {
    config = json_object();
  }

  resp.set("config", std::move(config));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("get-config", cmd_get_config, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
