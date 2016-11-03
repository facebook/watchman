/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

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
