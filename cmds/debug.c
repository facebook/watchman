/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* debug-recrawl */
void cmd_debug_recrawl(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-recrawl'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);

  if (!root) {
    return;
  }

  resp = make_response();

  w_root_lock(root);
  w_root_schedule_recrawl(root, "debug-recrawl");
  w_root_unlock(root);

  set_prop(resp, "recrawl", json_true());
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}

/* vim:ts=2:sw=2:et:
 */

