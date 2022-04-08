/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

bool try_client_mode_command(const Command& command, bool pretty) {
  auto client = std::make_shared<watchman::Client>();
  client->client_mode = true;

  bool res = client->dispatchCommand(command, CMD_CLIENT);

  if (!client->responses.empty()) {
    json_dumpf(
        client->responses.front(),
        stdout,
        pretty ? JSON_INDENT(4) : JSON_COMPACT);
    printf("\n");
  }

  return res;
}

/* vim:ts=2:sw=2:et:
 */
