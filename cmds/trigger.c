/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* trigger-del /root triggername
 * Delete a trigger from a root
 */
void cmd_trigger_delete(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  const char *name;
  w_string_t *tname;
  bool res;

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments");
    w_root_delref(root);
    return;
  }
  name = json_string_value(json_array_get(args, 2));
  if (!name) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    w_root_delref(root);
    return;
  }
  tname = w_string_new(name);

  w_root_lock(root);
  res = w_ht_del(root->commands, w_ht_ptr_val(tname));
  w_root_unlock(root);

  w_state_save();

  w_string_delref(tname);

  resp = make_response();
  set_prop(resp, "deleted", json_boolean(res));
  set_prop(resp, "trigger", json_string_nocheck(name));
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
void cmd_trigger_list(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  json_t *resp;
  json_t *arr;

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  resp = make_response();
  w_root_lock(root);
  arr = w_root_trigger_list_to_json(root);
  w_root_unlock(root);

  set_prop(resp, "triggers", arr);
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}

/* trigger /root triggername [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
void cmd_trigger(struct watchman_client *client, json_t *args)
{
  struct watchman_rule *rules;
  w_root_t *root;
  uint32_t next_arg = 0;
  struct watchman_trigger_command *cmd;
  json_t *resp;
  const char *name;
  char buf[128];

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments");
    goto done;
  }
  name = json_string_value(json_array_get(args, 2));
  if (!name) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    goto done;
  }

  if (!parse_watch_params(3, args, &rules, &next_arg, buf, sizeof(buf))) {
    send_error_response(client, "invalid rule spec: %s", buf);
    goto done;
  }

  if (next_arg >= json_array_size(args)) {
    send_error_response(client, "no command was specified");
    goto done;
  }

  cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    send_error_response(client, "no memory!");
    goto done;
  }

  cmd->rules = rules;
  cmd->argc = json_array_size(args) - next_arg;
  cmd->argv = w_argv_copy_from_json(args, next_arg);
  if (!cmd->argv) {
    free(cmd);
    send_error_response(client, "unable to build argv array");
    goto done;
  }

  cmd->triggername = w_string_new(name);
  w_root_lock(root);
  w_ht_replace(root->commands, w_ht_ptr_val(cmd->triggername),
      w_ht_ptr_val(cmd));
  w_root_unlock(root);

  w_state_save();

  resp = make_response();
  set_prop(resp, "triggerid", json_string_nocheck(name));
  send_and_dispose_response(client, resp);
done:
  w_root_delref(root);
}


/* vim:ts=2:sw=2:et:
 */

