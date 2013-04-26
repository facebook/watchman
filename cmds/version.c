/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* version */
void cmd_version(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  send_and_dispose_response(client, resp);
}

/* vim:ts=2:sw=2:et:
 */

