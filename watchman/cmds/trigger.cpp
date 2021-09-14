/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/CommandRegistry.h"
#include "watchman/Logging.h"
#include "watchman/Shutdown.h"
#include "watchman/TriggerCommand.h"
#include "watchman/sockname.h"
#include "watchman/state.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_root.h"
#include "watchman/watchman_stream.h"

#include <memory>

using namespace watchman;

/* trigger-del /root triggername
 * Delete a trigger from a root
 */
static void cmd_trigger_delete(
    struct watchman_client* client,
    const json_ref& args) {
  w_string tname;
  bool res;

  auto root = resolveRoot(client, args);

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments");
    return;
  }
  auto jname = args.at(2);
  if (!jname.isString()) {
    send_error_response(client, "expected 2nd parameter to be trigger name");
    return;
  }
  tname = json_to_w_string(jname);

  std::unique_ptr<TriggerCommand> cmd;

  {
    auto map = root->triggers.wlock();
    auto it = map->find(tname);
    if (it == map->end()) {
      res = false;
    } else {
      std::swap(cmd, it->second);
      map->erase(it);
      res = true;
    }
  }

  if (cmd) {
    // Stop the thread
    cmd->stop();
  }

  if (res) {
    w_state_save();
  }

  auto resp = make_response();
  resp.set({{"deleted", json_boolean(res)}, {"trigger", json_ref(jname)}});
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("trigger-del", cmd_trigger_delete, CMD_DAEMON, w_cmd_realpath_root)

/* trigger-list /root
 * Displays a list of registered triggers for a given root
 */
static void cmd_trigger_list(
    struct watchman_client* client,
    const json_ref& args) {
  auto root = resolveRoot(client, args);

  auto resp = make_response();
  auto arr = root->triggerListToJson();

  resp.set("triggers", std::move(arr));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("trigger-list", cmd_trigger_list, CMD_DAEMON, w_cmd_realpath_root)

static json_ref build_legacy_trigger(
    const std::shared_ptr<watchman_root>& root,
    struct watchman_client* client,
    const json_ref& args) {
  uint32_t next_arg = 0;
  uint32_t i;
  size_t n;

  auto trig = json_object(
      {{"name", args.at(2)},
       {"append_files", json_true()},
       {"stdin",
        json_array(
            {typed_string_to_json("name"),
             typed_string_to_json("exists"),
             typed_string_to_json("new"),
             typed_string_to_json("size"),
             typed_string_to_json("mode")})}});

  json_ref expr;
  auto query = w_query_parse_legacy(root, args, 3, &next_arg, nullptr, &expr);
  query->request_id = w_string::build("trigger ", json_to_w_string(args.at(2)));

  json_object_set(trig, "expression", expr.get_default("expression"));

  if (next_arg >= args.array().size()) {
    send_error_response(client, "no command was specified");
    return nullptr;
  }

  n = json_array_size(args) - next_arg;
  auto command = json_array_of_size(n);
  for (i = 0; i < n; i++) {
    auto ele = args.at(i + next_arg);
    if (!ele.isString()) {
      send_error_response(client, "expected argument %d to be a string", i);
      return nullptr;
    }
    json_array_append(command, ele);
  }
  json_object_set_new(trig, "command", std::move(command));

  return trig;
}

/* trigger /root triggername [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(struct watchman_client* client, const json_ref& args) {
  bool need_save = true;
  std::unique_ptr<TriggerCommand> cmd;
  json_ref trig;
  json_ref resp;

  auto root = resolveRoot(client, args);

  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments");
    return;
  }

  trig = args.at(2);
  if (trig.isString()) {
    trig = build_legacy_trigger(root, client, args);
    if (!trig) {
      return;
    }
  }

  cmd = std::make_unique<TriggerCommand>(root, trig);

  resp = make_response();
  resp.set("triggerid", w_string_to_json(cmd->triggername));

  {
    auto wlock = root->triggers.wlock();
    auto& map = *wlock;
    auto& old = map[cmd->triggername];

    if (old && json_equal(cmd->definition, old->definition)) {
      // Same definition: we don't and shouldn't touch things, so that we
      // preserve the associated trigger clock and don't cause the trigger
      // to re-run immediately
      resp.set(
          "disposition",
          typed_string_to_json("already_defined", W_STRING_UNICODE));
      need_save = false;
    } else {
      resp.set(
          "disposition",
          typed_string_to_json(old ? "replaced" : "created", W_STRING_UNICODE));
      if (old) {
        // If we're replacing an old definition, be sure to stop the old
        // one before we destroy it, and before we start the new one.
        old->stop();
      }
      // Start the new trigger thread
      cmd->start(root);
      old = std::move(cmd);

      need_save = true;
    }
  }

  if (need_save) {
    w_state_save();
  }

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("trigger", cmd_trigger, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
