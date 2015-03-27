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
W_CMD_REG("debug-recrawl", cmd_debug_recrawl, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_show_cursors(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp, *cursors;
  w_ht_iter_t i;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-show-cursors'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);

  if (!root) {
    return;
  }

  resp = make_response();

  w_root_lock(root);
  cursors = json_object_of_size(w_ht_size(root->cursors));
  if (w_ht_first(root->cursors, &i)) do {
    w_string_t *name = w_ht_val_ptr(i.key);
    set_prop(cursors, name->buf, json_integer(i.value));
  } while (w_ht_next(root->cursors, &i));
  w_root_unlock(root);

  set_prop(resp, "cursors", cursors);
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("debug-show-cursors", cmd_debug_show_cursors,
    CMD_DAEMON, w_cmd_realpath_root)

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
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_poison(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  struct timeval now;
  struct watchman_dir *dir;
  json_t *resp;

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  dir = w_root_resolve_dir(root, root->root_path, false);

  gettimeofday(&now, NULL);

  set_poison_state(root, dir, now, "debug-poison", ENOMEM, NULL);

  resp = make_response();
  set_prop(resp, "poison", json_string_nocheck(poisoned_reason));
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("debug-poison", cmd_debug_poison, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
