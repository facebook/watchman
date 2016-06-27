/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool try_client_mode_command(json_t *cmd, bool pretty)
{
  struct watchman_client client;
  bool res;
  struct watchman_client_response *resp;

  memset(&client, 0, sizeof(client));
  client.client_mode = true;
  res = dispatch_command(&client, cmd, CMD_CLIENT);

  resp = client.head;
  if (resp) {
    json_dumpf(resp->json, stdout,
               (size_t)(pretty ? JSON_INDENT(4) : JSON_COMPACT));
    printf("\n");
  }

  return res;
}

/* vim:ts=2:sw=2:et:
 */
