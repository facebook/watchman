/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/String.h>
#include <map>
#include <optional>
#include <set>
#include "watchman/Client.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Logging.h"
#include "watchman/root/Root.h"
#include "watchman/root/resolve.h"
#include "watchman/sockname.h"
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_stream.h"

using namespace watchman;

namespace {

class VersionCommand : public PrettyCommand<VersionCommand> {
 public:
  static constexpr std::string_view name = "version";

  static constexpr CommandFlags flags =
      CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER;

  struct Request {
    std::vector<w_string> required;
    std::vector<w_string> optional;

    static Request fromJson(const json_ref& args) {
      Request result;

      auto& arr = args.array();
      if (arr.size() == 0) {
        return result;
      }

      auto& params = arr[0];
      auto req_cap = params.get_default("required");
      auto opt_cap = params.get_default("optional");
      if (req_cap && req_cap.isArray()) {
        auto& req_vec = req_cap.array();
        result.required.reserve(req_vec.size());
        for (auto& cap : req_vec) {
          result.required.push_back(cap.asString());
        }
      }
      if (opt_cap && opt_cap.isArray()) {
        auto& opt_vec = opt_cap.array();
        result.required.reserve(opt_vec.size());
        for (auto& cap : opt_vec) {
          result.optional.push_back(cap.asString());
        }
      }

      return result;
    }
  };

  struct Response {
    // TODO: should this be automatic?
    w_string version;
    std::optional<w_string> buildinfo;
    std::map<w_string, bool> capabilities;
    std::optional<w_string> error;

    json_ref toJson() const {
      auto response = json_object();
      response.set("version", json::to(version));
      if (buildinfo) {
        response.set("buildinfo", json::to(buildinfo.value()));
      }
      if (!capabilities.empty()) {
        response.set("capabilities", json::to(capabilities));
      }
      if (error) {
        response.set("error", json::to(error.value()));
      }
      return response;
    }

    static Response fromJson(const json_ref& args) {
      Response result;
      result.version = json::from<w_string>(args.get("version"));
      result.buildinfo = args.get_default("buildinfo").asOptionalString();
      auto caps = args.get_default("capabilities");
      if (caps) {
        result.capabilities = json::from<std::map<w_string, bool>>(caps);
      }
      result.error = args.get_default("error").asOptionalString();
      return result;
    }
  };

  static Response handle(const Request& request) {
    Response response;

    response.version = w_string{PACKAGE_VERSION, W_STRING_UNICODE};
#ifdef WATCHMAN_BUILD_INFO
    response.buildinfo = w_string{WATCHMAN_BUILD_INFO, W_STRING_UNICODE};
#endif

    if (!request.required.empty() || !request.optional.empty()) {
      auto cap_res = json_object_of_size(
          request.required.size() + request.optional.size());

      for (const auto& capname : request.optional) {
        response.capabilities[capname] = capability_supported(capname.view());
      }

      std::set<w_string> missing;

      for (const auto& capname : request.required) {
        bool have = capability_supported(capname.view());
        response.capabilities[capname] = have;
        if (!have) {
          missing.insert(capname);
        }
      }

      if (!missing.empty()) {
        response.error = w_string::build(
            "client required capabilities [",
            folly::join(", ", missing),
            "] not supported by this server");
      }
    }

    return response;
  }

  static void printResult(const Response& response) {
    if (response.error) {
      fmt::print("error: {}\n", response.error.value());
    }
    fmt::print("version: {}\n", response.version);
    if (response.buildinfo) {
      fmt::print("buildinfo: {}\n", response.buildinfo.value());
    }
  }
};

WATCHMAN_COMMAND(version, VersionCommand);

/* list-capabilities */
static json_ref cmd_list_capabilities(Client*, const json_ref&) {
  auto resp = make_response();

  resp.set("capabilities", capability_get_list());
  return resp;
}
W_CMD_REG(
    "list-capabilities",
    cmd_list_capabilities,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    NULL);

/* get-sockname */
static json_ref cmd_get_sockname(Client*, const json_ref&) {
  auto resp = make_response();

  // For legacy reasons we report the unix domain socket as sockname on
  // unix but the named pipe path on windows
  resp.set(
      "sockname",
      w_string_to_json(w_string(get_sock_name_legacy(), W_STRING_BYTE)));
  if (!disable_unix_socket) {
    resp.set(
        "unix_domain", w_string_to_json(w_string::build(get_unix_sock_name())));
  }

#ifdef WIN32
  if (!disable_named_pipe) {
    resp.set(
        "named_pipe",
        w_string_to_json(w_string::build(get_named_pipe_sock_path())));
  }
#endif

  return resp;
}
W_CMD_REG(
    "get-sockname",
    cmd_get_sockname,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    NULL);

static json_ref cmd_get_config(Client* client, const json_ref& args) {
  json_ref config;

  if (json_array_size(args) != 2) {
    client->sendErrorResponse("wrong number of arguments for 'get-config'");
    return nullptr;
  }

  auto root = resolveRoot(client, args);

  auto resp = make_response();

  config = root->config_file;

  if (!config) {
    config = json_object();
  }

  resp.set("config", std::move(config));
  return resp;
}
W_CMD_REG("get-config", cmd_get_config, CMD_DAEMON, w_cmd_realpath_root);

} // namespace

/* vim:ts=2:sw=2:et:
 */
