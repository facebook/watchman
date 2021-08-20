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
