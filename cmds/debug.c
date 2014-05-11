/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void cmd_debug_recrawl(struct watchman_client *client, json_t *args)
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
W_CMD_REG("debug-recrawl", cmd_debug_recrawl, CMD_DAEMON)

/* debug-ageout */
static void cmd_debug_ageout(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  int min_age;

  /* resolve the root */
  if (json_array_size(args) != 3) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-ageout'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);

  if (!root) {
    return;
  }

  min_age = json_integer_value(json_array_get(args, 2));

  resp = make_response();

  w_root_lock(root);
  w_root_perform_age_out(root, min_age);
  w_root_unlock(root);

  set_prop(resp, "ageout", json_true());
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON)

/* vim:ts=2:sw=2:et:
 */
