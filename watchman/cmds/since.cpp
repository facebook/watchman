/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/Query.h"
#include "watchman/query/eval.h"
#include "watchman/query/parse.h"
#include "watchman/watchman_client.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

/* since /root <timestamp> [patterns] */
static void cmd_since(struct watchman_client* client, const json_ref& args) {
  const char* clockspec;

  /* resolve the root */
  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments for 'since'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto clock_ele = json_array_get(args, 2);
  clockspec = json_string_value(clock_ele);
  if (!clockspec) {
    send_error_response(client, "expected argument 2 to be a valid clockspec");
    return;
  }

  auto query = w_query_parse_legacy(root, args, 3, nullptr, clockspec, nullptr);
  query->clientPid = client->stm ? client->stm->getPeerProcessID() : 0;

  auto res = w_query_execute(query.get(), root, nullptr);
  auto response = make_response();
  response.set(
      {{"is_fresh_instance", json_boolean(res.isFreshInstance)},
       {"clock", res.clockAtStartOfQuery.toJson()},
       {"files", std::move(res.resultsArray)},
       {"debug", res.debugInfo.render()}});
  if (res.savedStateInfo) {
    response.set({{"saved-state-info", std::move(res.savedStateInfo)}});
  }

  add_root_warnings_to_response(response, root);
  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG(
    "since",
    cmd_since,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
