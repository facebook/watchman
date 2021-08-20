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

using namespace watchman;

bool try_client_mode_command(const json_ref& cmd, bool pretty) {
  auto client = std::make_shared<watchman_client>();
  bool res;

  client->client_mode = true;
  res = dispatch_command(client.get(), cmd, CMD_CLIENT);

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
