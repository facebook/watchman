/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Defines which commands we can run without the full service
 * being available */

static struct watchman_command_handler_def client_commands[] = {
  { "query", cmd_query },
  { NULL, NULL }
};

bool try_client_mode_command(json_t *cmd, bool pretty)
{
  struct watchman_client client;
  bool res;
  struct watchman_client_response *resp;

  register_commands(client_commands);

  memset(&client, 0, sizeof(client));
  client.client_mode = true;
  res = dispatch_command(&client, cmd);

  resp = client.head;
  if (resp) {
    json_dumpf(resp->json, stdout, pretty ? JSON_INDENT(4) : JSON_COMPACT);
    printf("\n");
  }

  return res;
}



/* vim:ts=2:sw=2:et:
 */

