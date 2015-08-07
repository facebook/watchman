/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* trigger-del /root triggername
 * Delete a trigger from a root
 */
static void cmd_trigger_delete(struct watchman_client *client, json_t *args)
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

  if (res) {
    w_state_save();
  }

  w_string_delref(tname);

  resp = make_response();
  set_prop(resp, "deleted", json_boolean(res));
  set_prop(resp, "trigger", json_string_nocheck(name));
  send_and_dispose_response(client, resp);
  w_root_delref(root);
}
W_CMD_REG("trigger-del", cmd_trigger_delete, CMD_DAEMON, w_cmd_realpath_root)

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
static void cmd_trigger_list(struct watchman_client *client, json_t *args)
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
W_CMD_REG("trigger-list", cmd_trigger_list, CMD_DAEMON, w_cmd_realpath_root)

static json_t *build_legacy_trigger(
  w_root_t *root,
  struct watchman_client *client,
  json_t *args)
{
  json_t *trig, *expr;
  json_t *command;
  char *errmsg;
  uint32_t next_arg = 0;
  uint32_t i;
  size_t n;
  w_query *query;

  trig = json_pack("{s:O, s:b, s:[s, s, s, s, s]}",
    "name", json_array_get(args, 2),
    "append_files", true,
    "stdin",
      // [
      "name", "exists", "new", "size", "mode"
      // ]
  );

  query = w_query_parse_legacy(root, args, &errmsg, 3, &next_arg, NULL, &expr);
  if (!query) {
    send_error_response(client, "invalid rule spec: %s", errmsg);
    free(errmsg);
    json_decref(trig);
    return NULL;
  }
  w_query_delref(query);

  json_object_set(trig, "expression", json_object_get(expr, "expression"));
  json_decref(expr);

  if (next_arg >= json_array_size(args)) {
    send_error_response(client, "no command was specified");
    json_decref(trig);
    return NULL;
  }

  n = json_array_size(args) - next_arg;
  command = json_array_of_size(n);
  for (i = 0; i < n; i++) {
    json_t *ele = json_array_get(args, i + next_arg);
    if (!json_is_string(ele)) {
      send_error_response(client, "expected argument %d to be a string", i);
      json_decref(trig);
      return NULL;
    }
    json_array_append(command, ele);
  }
  json_object_set_new(trig, "command", command);

  return trig;
}

static bool parse_redirection(const char **name_p, int *flags,
  const char *label, char **errmsg)
{
  const char *name = *name_p;

  if (!name) {
    return true;
  }

  if (name[0] != '>') {
    ignore_result(asprintf(errmsg,
      "%s: must be prefixed with either > or >>, got %s",
      label, name));
    return false;
  }

  *flags = O_CREAT|O_CLOEXEC|O_WRONLY;

  if (name[1] == '>') {
#ifdef _WIN32
    ignore_result(asprintf(errmsg,
      "Windows does not support O_APPEND"));
    return false;
#else
    *flags |= O_APPEND;
    *name_p = name + 2;
#endif
  } else {
    *flags |= O_TRUNC;
    *name_p = name + 1;
  }

  return true;
}

void w_trigger_command_free(struct watchman_trigger_command *cmd)
{
  if (cmd->triggername) {
    w_string_delref(cmd->triggername);
  }

  if (cmd->command) {
    json_decref(cmd->command);
  }

  if (cmd->definition) {
    json_decref(cmd->definition);
  }

  if (cmd->query) {
    w_query_delref(cmd->query);
  }

  if (cmd->envht) {
    w_ht_free(cmd->envht);
  }

  free(cmd);
}

struct watchman_trigger_command *w_build_trigger_from_def(
  w_root_t *root, json_t *trig, char **errmsg)
{
  struct watchman_trigger_command *cmd;
  json_t *ele, *query, *relative_root;
  json_int_t jint;
  const char *name = NULL;

  cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    *errmsg = strdup("no memory");
    return NULL;
  }

  cmd->definition = trig;
  json_incref(cmd->definition);

  query = json_pack("{s:O}", "expression",
      json_object_get(cmd->definition, "expression"));
  relative_root = json_object_get(cmd->definition, "relative_root");
  if (relative_root) {
    json_object_set_nocheck(query, "relative_root", relative_root);
  }

  cmd->query = w_query_parse(root, query, errmsg);
  json_decref(query);

  if (!cmd->query) {
    w_trigger_command_free(cmd);
    return NULL;
  }

  json_unpack(trig, "{s:s}", "name", &name);
  if (!name) {
    *errmsg = strdup("invalid or missing name");
    w_trigger_command_free(cmd);
    return NULL;
  }

  cmd->triggername = w_string_new(name);
  cmd->command = json_object_get(trig, "command");
  if (cmd->command) {
    json_incref(cmd->command);
  }
  if (!cmd->command || !json_is_array(cmd->command) ||
      !json_array_size(cmd->command)) {
    *errmsg = strdup("invalid command array");
    w_trigger_command_free(cmd);
    return NULL;
  }

  json_unpack(trig, "{s:b}", "append_files", &cmd->append_files);

  ele = json_object_get(trig, "stdin");
  if (!ele) {
    cmd->stdin_style = input_dev_null;
  } else if (json_is_array(ele)) {
    cmd->stdin_style = input_json;
    if (!parse_field_list(ele, &cmd->field_list, errmsg)) {
      w_trigger_command_free(cmd);
      return NULL;
    }
  } else if (json_is_string(ele)) {
    const char *str = json_string_value(ele);
    if (!strcmp(str, "/dev/null")) {
      cmd->stdin_style = input_dev_null;
    } else if (!strcmp(str, "NAME_PER_LINE")) {
      cmd->stdin_style = input_name_list;
    } else {
      ignore_result(asprintf(errmsg, "invalid stdin value %s", str));
      w_trigger_command_free(cmd);
      return NULL;
    }
  } else {
    *errmsg = strdup("invalid value for stdin");
    w_trigger_command_free(cmd);
    return NULL;
  }

  jint = 0; // unlimited unless specified
  json_unpack(trig, "{s:I}", "max_files_stdin", &jint);
  if (jint < 0) {
    *errmsg = strdup("max_files_stdin must be >= 0");
    w_trigger_command_free(cmd);
    return NULL;
  }
  cmd->max_files_stdin = (uint32_t)jint;

  json_unpack(trig, "{s:s}", "stdout", &cmd->stdout_name);
  json_unpack(trig, "{s:s}", "stderr", &cmd->stderr_name);

  if (!parse_redirection(&cmd->stdout_name, &cmd->stdout_flags,
        "stdout", errmsg)) {
    w_trigger_command_free(cmd);
    return NULL;
  }

  if (!parse_redirection(&cmd->stderr_name, &cmd->stderr_flags,
        "stderr", errmsg)) {
    w_trigger_command_free(cmd);
    return NULL;
  }

  // Copy current environment
  cmd->envht = w_envp_make_ht();

  // Set some standard vars
  w_envp_set(cmd->envht, "WATCHMAN_ROOT", root->root_path);
  w_envp_set_cstring(cmd->envht, "WATCHMAN_SOCK", get_sock_name());
  w_envp_set(cmd->envht, "WATCHMAN_TRIGGER", cmd->triggername);

  return cmd;
}

/* trigger /root triggername [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  struct watchman_trigger_command *cmd, *old;
  json_t *resp;
  json_t *trig;
  char *errmsg = NULL;
  bool need_save = true;

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments");
    goto done;
  }

  trig = json_array_get(args, 2);
  if (json_is_string(trig)) {
    trig = build_legacy_trigger(root, client, args);
    if (!trig) {
      goto done;
    }
  } else {
    // Add a ref so that we don't need to conditionally decref later
    // for the legacy case later
    json_incref(trig);
  }

  cmd = w_build_trigger_from_def(root, trig, &errmsg);
  json_decref(trig);

  if (!cmd) {
    send_error_response(client, "%s", errmsg);
    goto done;
  }

  resp = make_response();
  set_prop(resp, "triggerid", json_string_nocheck(cmd->triggername->buf));

  w_root_lock(root);

  old = w_ht_val_ptr(w_ht_get(root->commands,
          w_ht_ptr_val(cmd->triggername)));
  if (old && json_equal(cmd->definition, old->definition)) {
    // Same definition: we don't and shouldn't touch things, so that we
    // preserve the associated trigger clock and don't cause the trigger
    // to re-run immediately
    set_prop(resp, "disposition", json_string_nocheck("already_defined"));
    w_trigger_command_free(cmd);
    cmd = NULL;
    need_save = false;
  } else {
    set_prop(resp, "disposition", json_string_nocheck(
          old ? "replaced" : "created"));
    w_ht_replace(root->commands, w_ht_ptr_val(cmd->triggername),
        w_ht_ptr_val(cmd));
    // Force the trigger to be eligible to run now
    root->ticks++;
    root->pending_trigger_tick = root->ticks;
  }
  w_root_unlock(root);

  if (need_save) {
    w_state_save();
  }

  send_and_dispose_response(client, resp);

done:
  if (errmsg) {
    free(errmsg);
  }
  w_root_delref(root);
}
W_CMD_REG("trigger", cmd_trigger, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
