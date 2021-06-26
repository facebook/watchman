/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/chrono/Conv.h>
#include <iomanip>
#include "watchman/Logging.h"
#include "watchman/watchman.h"

using namespace watchman;

static void cmd_debug_recrawl(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-recrawl'");
    return;
  }

  auto root = resolveRoot(client, args);

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
    send_error_response(
        client, "wrong number of arguments for 'debug-show-cursors'");
    return;
  }

  auto root = resolveRoot(client, args);

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
W_CMD_REG(
    "debug-show-cursors",
    cmd_debug_show_cursors,
    CMD_DAEMON,
    w_cmd_realpath_root)

/* debug-ageout */
static void cmd_debug_ageout(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments for 'debug-ageout'");
    return;
  }

  auto root = resolveRoot(client, args);

  std::chrono::seconds min_age(json_array_get(args, 2).asInt());

  auto resp = make_response();

  root->performAgeOut(min_age);

  resp.set("ageout", json_true());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_poison(
    struct watchman_client* client,
    const json_ref& args) {
  auto root = resolveRoot(client, args);

  auto now = std::chrono::system_clock::now();

  set_poison_state(
      root->root_path,
      now,
      "debug-poison",
      std::error_code(ENOMEM, std::generic_category()));

  auto resp = make_response();
  resp.set(
      "poison",
      typed_string_to_json(poisoned_reason.rlock()->c_str(), W_STRING_UNICODE));
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
W_CMD_REG("debug-drop-privs", cmd_debug_drop_privs, CMD_DAEMON, NULL)

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
    if (!it.second.isBool()) {
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
    bool new_paused = it.second.asBool();
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

static json_ref getDebugSubscriptionInfo(watchman_root* root) {
  auto subscriptions = json_array();
  for (const auto& c : *::clients.rlock()) {
    auto* user_client = dynamic_cast<watchman_user_client*>(c.get());
    if (!user_client) {
      continue;
    }
    for (const auto& sub : user_client->subscriptions) {
      if (root == sub.second->root.get()) {
        auto last_responses = json_array();
        for (auto& response : sub.second->lastResponses) {
          char timebuf[64];
          last_responses.array().push_back(json_object({
              {"written_time",
               typed_string_to_json(Log::timeString(
                   timebuf,
                   std::size(timebuf),
                   folly::to<timeval>(response.written)))},
              {"response", response.response},
          }));
        }

        subscriptions.array().push_back(json_object({
            {"name", w_string_to_json(sub.first)},
            {"client_id", json_integer(user_client->unique_id)},
            {"last_responses", last_responses},
        }));
      }
    }
  }
  return subscriptions;
}

static void cmd_debug_get_subscriptions(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto client = (watchman_user_client*)clientbase;

  auto root = resolveRoot(client, args);

  auto resp = make_response();
  auto debug_info = root->unilateralResponses->getDebugInfo();
  // copy over all the key-value pairs from debug_info
  resp.object().insert(debug_info.object().begin(), debug_info.object().end());

  auto subscriptions = getDebugSubscriptionInfo(root.get());
  resp.object().emplace("subscriptions", subscriptions);

  send_and_dispose_response(clientbase, std::move(resp));
}
W_CMD_REG(
    "debug-get-subscriptions",
    cmd_debug_get_subscriptions,
    CMD_DAEMON,
    w_cmd_realpath_root)

static void cmd_debug_get_asserted_states(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto client = (watchman_user_client*)clientbase;

  auto root = resolveRoot(client, args);
  auto response = make_response();

  // copy over all the key-value pairs to stateSet and release lock
  auto states = root->assertedStates.rlock()->debugStates();
  response.set(
      {{"root", w_string_to_json(root->root_path)},
       {"states", std::move(states)}});
  send_and_dispose_response(clientbase, std::move(response));
}
W_CMD_REG(
    "debug-get-asserted-states",
    cmd_debug_get_asserted_states,
    CMD_DAEMON,
    w_cmd_realpath_root)

static void cmd_debug_status(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();
  auto roots = watchman_root::getStatusForAllRoots();
  resp.set("roots", std::move(roots));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "debug-status",
    cmd_debug_status,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    NULL)

static void cmd_debug_watcher_info(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto* client = static_cast<watchman_user_client*>(clientbase);

  auto root = resolveRoot(client, args);
  auto response = make_response();
  response.set("watcher-debug-info", root->view()->getWatcherDebugInfo());
  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG("debug-watcher-info", cmd_debug_watcher_info, CMD_DAEMON, NULL)

static void cmd_debug_watcher_info_clear(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto* client = static_cast<watchman_user_client*>(clientbase);

  auto root = resolveRoot(client, args);
  auto response = make_response();
  root->view()->clearWatcherDebugInfo();
  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG(
    "debug-watcher-info-clear",
    cmd_debug_watcher_info_clear,
    CMD_DAEMON,
    NULL)

/* vim:ts=2:sw=2:et:
 */
