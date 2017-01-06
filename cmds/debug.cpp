/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void cmd_debug_recrawl(
    struct watchman_client* client,
    const json_ref& args) {

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-recrawl'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto resp = make_response();

  root->scheduleRecrawl("debug-recrawl");

  resp.set("recrawl", json_true());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-recrawl", cmd_debug_recrawl, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_show_cursors(
    struct watchman_client* client,
    const json_ref& args) {
  json_ref cursors;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-show-cursors'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto resp = make_response();

  {
    auto map = root->inner.cursors.rlock();
    cursors = json_object_of_size(map->size());
    for (const auto& it : *map) {
      const auto& name = it.first;
      const auto& ticks = it.second;
      cursors.set(name.c_str(), json_integer(ticks));
    }
  }

  resp.set("cursors", std::move(cursors));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-show-cursors", cmd_debug_show_cursors,
    CMD_DAEMON, w_cmd_realpath_root)

/* debug-ageout */
static void cmd_debug_ageout(
    struct watchman_client* client,
    const json_ref& args) {

  /* resolve the root */
  if (json_array_size(args) != 3) {
    send_error_response(client,
                        "wrong number of arguments for 'debug-ageout'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  std::chrono::seconds min_age(json_integer_value(json_array_get(args, 2)));

  auto resp = make_response();

  root->performAgeOut(min_age);

  resp.set("ageout", json_true());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_poison(
    struct watchman_client* client,
    const json_ref& args) {
  struct timeval now;

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  gettimeofday(&now, NULL);

  set_poison_state(
      root->root_path,
      now,
      "debug-poison",
      std::error_code(ENOMEM, std::generic_category()));

  auto resp = make_response();
  resp.set("poison", typed_string_to_json(poisoned_reason, W_STRING_UNICODE));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-poison", cmd_debug_poison, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_drop_privs(
    struct watchman_client* client,
    const json_ref&) {
  client->client_is_owner = false;

  auto resp = make_response();
  resp.set("owner", json_boolean(client->client_is_owner));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-drop-privs", cmd_debug_drop_privs, CMD_DAEMON, NULL);

static void cmd_debug_set_subscriptions_paused(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto client = (struct watchman_user_client*)clientbase;

  const auto& paused = args.at(1);
  auto& paused_map = paused.object();
  for (auto& it : paused_map) {
    auto sub_iter = client->subscriptions.find(it.first);
    if (sub_iter == client->subscriptions.end()) {
      send_error_response(
          client,
          "this client does not have a subscription named '%s'",
          it.first.c_str());
      return;
    }
    if (!json_is_boolean(it.second)) {
      send_error_response(
          client,
          "new value for subscription '%s' not a boolean",
          it.first.c_str());
      return;
    }
  }

  auto states = json_object();

  for (auto& it : paused_map) {
    auto sub_iter = client->subscriptions.find(it.first);
    bool old_paused = sub_iter->second->debug_paused;
    bool new_paused = json_is_true(it.second);
    sub_iter->second->debug_paused = new_paused;
    states.set(
        it.first,
        json_object({{"old", json_boolean(old_paused)}, {"new", it.second}}));
  }

  auto resp = make_response();
  resp.set("paused", std::move(states));
  send_and_dispose_response(clientbase, std::move(resp));
}
W_CMD_REG(
    "debug-set-subscriptions-paused",
    cmd_debug_set_subscriptions_paused,
    CMD_DAEMON,
    nullptr)

static void cmd_debug_get_subscriptions(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto client = (watchman_user_client*)clientbase;

  auto root = resolve_root_or_err(client, args, 1, false);

  auto resp = make_response();
  auto debug_info = root->unilateralResponses->getDebugInfo();
  // copy over all the key-value pairs from debug_info
  resp.object().insert(debug_info.object().begin(), debug_info.object().end());
  send_and_dispose_response(clientbase, std::move(resp));
}
W_CMD_REG(
    "debug-get-subscriptions",
    cmd_debug_get_subscriptions,
    CMD_DAEMON,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
