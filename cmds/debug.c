/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void cmd_debug_recrawl(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  struct write_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-recrawl'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  resp = make_response();

  w_root_lock(&unlocked, "debug-recrawl", &lock);
  w_root_schedule_recrawl(lock.root, "debug-recrawl");
  w_root_unlock(&lock, &unlocked);

  set_prop(resp, "recrawl", json_true());
  send_and_dispose_response(client, resp);
  w_root_delref(unlocked.root);
}
W_CMD_REG("debug-recrawl", cmd_debug_recrawl, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_show_cursors(struct watchman_client *client, json_t *args)
{
  json_t *resp, *cursors;
  w_ht_iter_t i;
  struct write_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-show-cursors'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  resp = make_response();

  w_root_lock(&unlocked, "debug-show-cursors", &lock);
  cursors = json_object_of_size(w_ht_size(lock.root->cursors));
  if (w_ht_first(lock.root->cursors, &i)) do {
    w_string_t *name = w_ht_val_ptr(i.key);
    set_prop(cursors, name->buf, json_integer(i.value));
  } while (w_ht_next(lock.root->cursors, &i));
  w_root_unlock(&lock, &unlocked);

  set_prop(resp, "cursors", cursors);
  send_and_dispose_response(client, resp);
  w_root_delref(unlocked.root);
}
W_CMD_REG("debug-show-cursors", cmd_debug_show_cursors,
    CMD_DAEMON, w_cmd_realpath_root)

/* debug-ageout */
static void cmd_debug_ageout(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  int min_age;
  struct write_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) != 3) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-ageout'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  min_age = (int)json_integer_value(json_array_get(args, 2));

  resp = make_response();

  w_root_lock(&unlocked, "debug-ageout", &lock);
  w_root_perform_age_out(&lock, min_age);
  w_root_unlock(&lock, &unlocked);

  set_prop(resp, "ageout", json_true());
  send_and_dispose_response(client, resp);
  w_root_delref(unlocked.root);
}
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_poison(struct watchman_client *client, json_t *args)
{
  struct timeval now;
  json_t *resp;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  gettimeofday(&now, NULL);

  set_poison_state(unlocked.root, unlocked.root->root_path, now, "debug-poison",
                   ENOMEM, NULL);

  resp = make_response();
  set_unicode_prop(resp, "poison", poisoned_reason);
  send_and_dispose_response(client, resp);
  w_root_delref(unlocked.root);
}
W_CMD_REG("debug-poison", cmd_debug_poison, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_drop_privs(struct watchman_client *client, json_t *args)
{
  json_t *resp;

  unused_parameter(args);
  client->client_is_owner = false;

  resp = make_response();
  set_prop(resp, "owner", json_boolean(client->client_is_owner));
  send_and_dispose_response(client, resp);
}
W_CMD_REG("debug-drop-privs", cmd_debug_drop_privs, CMD_DAEMON, NULL);

/* vim:ts=2:sw=2:et:
 */
