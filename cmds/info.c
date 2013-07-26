/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* version */
void cmd_version(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

#ifdef WATCHMAN_BUILD_INFO
  set_prop(resp, "buildinfo", json_string(WATCHMAN_BUILD_INFO));
#endif

  send_and_dispose_response(client, resp);
}

/* get-sockname */
void cmd_get_sockname(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  set_prop(resp, "sockname", json_string(get_sock_name()));

  send_and_dispose_response(client, resp);
}

/* get-pid */
void cmd_get_pid(struct watchman_client *client, json_t *args)
{
  json_t *resp = make_response();

  unused_parameter(args);

  set_prop(resp, "pid", json_integer(getpid()));

  send_and_dispose_response(client, resp);
}


/* vim:ts=2:sw=2:et:
 */

