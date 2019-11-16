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
      auto buf = w_string::build(
          "client required capability `",
          capname,
          "` is not supported by this server");
      response.set("error", w_string_to_json(buf));
      watchman::log(watchman::ERR, "version: ", buf, "\n");

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

    if (opt_cap && opt_cap.isArray()) {
      query_caps(resp, cap_res, opt_cap, false);
    }
    if (req_cap && req_cap.isArray()) {
      query_caps(resp, cap_res, req_cap, true);
    }

    resp.set("capabilities", std::move(cap_res));
  }

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "version",
    cmd_version,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    NULL)

/* list-capabilities */
static void cmd_list_capabilities(
    struct watchman_client* client,
    const json_ref&) {
  auto resp = make_response();

  resp.set("capabilities", w_capability_get_list());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "list-capabilities",
    cmd_list_capabilities,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    NULL)

/* get-sockname */
static void cmd_get_sockname(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();

  // For legacy reasons we report the unix domain socket as sockname on
  // unix but the named pipe path on windows

#ifdef WIN32
  auto named_pipe = w_string(get_sock_name(), W_STRING_BYTE);
  resp.set("sockname", w_string_to_json(named_pipe));
  resp.set("named_pipe", w_string_to_json(named_pipe));
#else
  auto unix_domain = w_string(get_sock_name(), W_STRING_BYTE);
  resp.set("sockname", w_string_to_json(unix_domain));
  resp.set("unix_domain", w_string_to_json(unix_domain));
#endif

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "get-sockname",
    cmd_get_sockname,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    NULL)

static void cmd_get_config(
    struct watchman_client* client,
    const json_ref& args) {
  json_ref config;

  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments for 'get-config'");
    return;
  }

  auto root = resolveRoot(client, args);

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
