/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

/* process any pending triggers.
 * This is called from the IO thread */
void process_triggers(struct write_locked_watchman_root *lock) {
  w_root_t *root = lock->root;

  auto mostRecent = root->inner.ticks;

  if (root->inner.last_trigger_tick == mostRecent) {
    return;
  }

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer triggers until
  // things settle down
  if (is_vcs_op_in_progress(lock)) {
    w_log(W_LOG_DBG, "deferring triggers until VCS operations complete\n");
    return;
  }

  w_log(
      W_LOG_DBG,
      "last=%" PRIu32 "  pending=%" PRIu32 "\n",
      root->inner.last_trigger_tick,
      mostRecent);

  /* walk the list of triggers, and run their rules */
  {
    auto map = root->triggers.rlock();
    for (const auto& it : *map) {
      const auto& cmd = it.second;

      if (cmd->current_proc) {
        // Don't spawn if there's one already running
        w_log(
            W_LOG_DBG,
            "process_triggers: %s is already running\n",
            cmd->triggername.c_str());
        continue;
      }

      w_assess_trigger(lock, cmd.get());
    }
  }

  root->inner.last_trigger_tick = mostRecent;
}

/* trigger-del /root triggername
 * Delete a trigger from a root
 */
static void cmd_trigger_delete(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  json_t *jname;
  w_string tname;
  bool res;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments");
    w_root_delref(&unlocked);
    return;
  }
  jname = json_array_get(args, 2);
  if (!json_is_string(jname)) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    w_root_delref(&unlocked);
    return;
  }
  tname = json_to_w_string(jname);

  {
    auto map = unlocked.root->triggers.wlock();
    auto it = map->find(tname);
    if (it == map->end()) {
      res = false;
    } else {
      map->erase(it);
      res = true;
    }
  }

  if (res) {
    w_state_save();
  }

  resp = make_response();
  set_prop(resp, "deleted", json_boolean(res));
  json_incref(jname);
  set_prop(resp, "trigger", jname);
  send_and_dispose_response(client, resp);
  w_root_delref(&unlocked);
}
W_CMD_REG("trigger-del", cmd_trigger_delete, CMD_DAEMON, w_cmd_realpath_root)

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
static void cmd_trigger_list(struct watchman_client *client, json_t *args)
{
  json_t *resp;
  json_t *arr;
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  resp = make_response();
  w_root_read_lock(&unlocked, "trigger-list", &lock);
  arr = w_root_trigger_list_to_json(&lock);
  w_root_read_unlock(&lock, &unlocked);

  set_prop(resp, "triggers", arr);
  send_and_dispose_response(client, resp);
  w_root_delref(&unlocked);
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
  std::shared_ptr<w_query> query;

  trig = json_pack("{s:O, s:b, s:[u, u, u, u, u]}",
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

watchman_trigger_command::~watchman_trigger_command() {
  if (command) {
    json_decref(command);
  }

  if (definition) {
    json_decref(definition);
  }

  if (envht) {
    w_ht_free(envht);
  }
}

watchman_trigger_command::watchman_trigger_command()
    : query(nullptr),
      definition(nullptr),
      command(nullptr),
      envht(nullptr),
      append_files(0),
      stdin_style(input_dev_null),
      max_files_stdin(0),
      stdout_flags(0),
      stderr_flags(0),
      stdout_name(nullptr),
      stderr_name(nullptr),
      current_proc(0) {}

std::unique_ptr<watchman_trigger_command>
w_build_trigger_from_def(const w_root_t* root, json_t* trig, char** errmsg) {
  json_t *ele, *query, *relative_root;
  json_int_t jint;
  const char *name = NULL;

  auto cmd = watchman::make_unique<watchman_trigger_command>();
  if (!cmd) {
    *errmsg = strdup("no memory");
    return nullptr;
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
    return nullptr;
  }

  json_unpack(trig, "{s:u}", "name", &name);
  if (!name) {
    *errmsg = strdup("invalid or missing name");
    return nullptr;
  }

  cmd->triggername = w_string(name, W_STRING_UNICODE);
  cmd->command = json_object_get(trig, "command");
  if (cmd->command) {
    json_incref(cmd->command);
  }
  if (!cmd->command || !json_is_array(cmd->command) ||
      !json_array_size(cmd->command)) {
    *errmsg = strdup("invalid command array");
    return nullptr;
  }

  json_unpack(trig, "{s:b}", "append_files", &cmd->append_files);

  ele = json_object_get(trig, "stdin");
  if (!ele) {
    cmd->stdin_style = input_dev_null;
  } else if (json_is_array(ele)) {
    cmd->stdin_style = input_json;
    if (!parse_field_list(ele, &cmd->field_list, errmsg)) {
      return nullptr;
    }
  } else if (json_is_string(ele)) {
    const char *str = json_string_value(ele);
    if (!strcmp(str, "/dev/null")) {
      cmd->stdin_style = input_dev_null;
    } else if (!strcmp(str, "NAME_PER_LINE")) {
      cmd->stdin_style = input_name_list;
    } else {
      ignore_result(asprintf(errmsg, "invalid stdin value %s", str));
      return nullptr;
    }
  } else {
    *errmsg = strdup("invalid value for stdin");
    return nullptr;
  }

  jint = 0; // unlimited unless specified
  json_unpack(trig, "{s:I}", "max_files_stdin", &jint);
  if (jint < 0) {
    *errmsg = strdup("max_files_stdin must be >= 0");
    return nullptr;
  }
  cmd->max_files_stdin = (uint32_t)jint;

  json_unpack(trig, "{s:s}", "stdout", &cmd->stdout_name);
  json_unpack(trig, "{s:s}", "stderr", &cmd->stderr_name);

  if (!parse_redirection(&cmd->stdout_name, &cmd->stdout_flags,
        "stdout", errmsg)) {
    return nullptr;
  }

  if (!parse_redirection(&cmd->stderr_name, &cmd->stderr_flags,
        "stderr", errmsg)) {
    return nullptr;
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
  json_t *resp;
  json_t *trig;
  char *errmsg = NULL;
  bool need_save = true;
  struct unlocked_watchman_root unlocked;
  std::unique_ptr<watchman_trigger_command> cmd;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments");
    goto done;
  }

  trig = json_array_get(args, 2);
  if (json_is_string(trig)) {
    trig = build_legacy_trigger(unlocked.root, client, args);
    if (!trig) {
      goto done;
    }
  } else {
    // Add a ref so that we don't need to conditionally decref later
    // for the legacy case later
    json_incref(trig);
  }

  cmd = w_build_trigger_from_def(unlocked.root, trig, &errmsg);
  json_decref(trig);

  if (!cmd) {
    send_error_response(client, "%s", errmsg);
    goto done;
  }

  resp = make_response();
  set_prop(resp, "triggerid", w_string_to_json(cmd->triggername));

  {
    auto wlock = unlocked.root->triggers.wlock();
    auto& map = *wlock;
    auto& old = map[cmd->triggername];

    if (old && json_equal(cmd->definition, old->definition)) {
      // Same definition: we don't and shouldn't touch things, so that we
      // preserve the associated trigger clock and don't cause the trigger
      // to re-run immediately
      set_unicode_prop(resp, "disposition", "already_defined");
      need_save = false;
    } else {
      set_unicode_prop(resp, "disposition", old ? "replaced" : "created");
      old = std::move(cmd);
      need_save = true;
    }
  }

  if (need_save) {
    struct write_locked_watchman_root lock;

    w_root_lock(&unlocked, "trigger-add", &lock);
    // Force the trigger to be eligible to run now
    lock.root->inner.ticks++;
    w_root_unlock(&lock, &unlocked);

    w_state_save();
  }

  send_and_dispose_response(client, resp);

done:
  if (errmsg) {
    free(errmsg);
  }
  w_root_delref(&unlocked);
}
W_CMD_REG("trigger", cmd_trigger, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
