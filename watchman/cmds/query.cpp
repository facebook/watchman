/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/Query.h"
#include "watchman/query/eval.h"
#include "watchman/watchman_client.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_query.h"

using namespace watchman;

/* query /root {query} */
static void cmd_query(struct watchman_client* client, const json_ref& args) {
  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments for 'query'");
    return;
  }

  auto root = resolveRoot(client, args);

  const auto& query_spec = args.at(2);
  auto query = w_query_parse(root, query_spec);
  query->clientPid = client->stm ? client->stm->getPeerProcessID() : 0;

  if (client->client_mode) {
    query->sync_timeout = std::chrono::milliseconds(0);
  }

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
    "query",
    cmd_query,
    CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
