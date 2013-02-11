/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* watch /root */
void cmd_watch(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  resp = make_response();
  set_prop(resp, "watch", json_string_nocheck(root->root_path->buf));
  send_and_dispose_response(client, resp);
}


/* vim:ts=2:sw=2:et:
 */

