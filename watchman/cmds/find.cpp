/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman_client.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_query.h"

using namespace watchman;

/* find /root [patterns] */
static void cmd_find(struct watchman_client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto query = w_query_parse_legacy(root, args, 2, nullptr, nullptr, nullptr);
  if (client->client_mode) {
    query->sync_timeout = std::chrono::milliseconds(0);
  }
  query->clientPid = client->stm ? client->stm->getPeerProcessID() : 0;

  auto res = w_query_execute(query.get(), root, nullptr);
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
