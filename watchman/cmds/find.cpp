/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/Query.h"
#include "watchman/query/eval.h"
#include "watchman/query/parse.h"
#include "watchman/saved_state/SavedStateFactory.h"
#include "watchman/watchman_client.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

/* find /root [patterns] */
static void cmd_find(struct watchman_client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto query = parseQueryLegacy(root, args, 2, nullptr, nullptr, nullptr);
  if (client->client_mode) {
    query->sync_timeout = std::chrono::milliseconds(0);
  }
  query->clientPid = client->stm ? client->stm->getPeerProcessID() : 0;

  auto res = w_query_execute(query.get(), root, nullptr, getInterface);
  auto response = make_response();
  response.set(
      {{"clock", res.clockAtStartOfQuery.toJson()},
       {"files", std::move(res.resultsArray)}});

  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG(
    "find",
    cmd_find,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
