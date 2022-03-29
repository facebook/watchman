/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/chrono/Conv.h>
#include <iomanip>
#include "watchman/Client.h"
#include "watchman/InMemoryView.h"
#include "watchman/LRUCache.h"
#include "watchman/Logging.h"
#include "watchman/Poison.h"
#include "watchman/QueryableView.h"
#include "watchman/root/Root.h"
#include "watchman/watchman_cmd.h"

using namespace watchman;

static void cmd_debug_recrawl(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    client->sendErrorResponse("wrong number of arguments for 'debug-recrawl'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto resp = make_response();

  root->scheduleRecrawl("debug-recrawl");

  resp.set("recrawl", json_true());
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG("debug-recrawl", cmd_debug_recrawl, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_show_cursors(Client* client, const json_ref& args) {
  json_ref cursors;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    client->sendErrorResponse(
        "wrong number of arguments for 'debug-show-cursors'");
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
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-show-cursors",
    cmd_debug_show_cursors,
    CMD_DAEMON,
    w_cmd_realpath_root)

/* debug-ageout */
static void cmd_debug_ageout(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 3) {
    client->sendErrorResponse("wrong number of arguments for 'debug-ageout'");
    return;
  }

  auto root = resolveRoot(client, args);

  std::chrono::seconds min_age(json_array_get(args, 2).asInt());

  auto resp = make_response();

  root->performAgeOut(min_age);

  resp.set("ageout", json_true());
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG("debug-ageout", cmd_debug_ageout, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_poison(Client* client, const json_ref& args) {
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
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG("debug-poison", cmd_debug_poison, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_debug_drop_privs(Client* client, const json_ref&) {
  client->client_is_owner = false;

  auto resp = make_response();
  resp.set("owner", json_boolean(client->client_is_owner));
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG("debug-drop-privs", cmd_debug_drop_privs, CMD_DAEMON, NULL)

static void cmd_debug_set_subscriptions_paused(
    Client* clientbase,
    const json_ref& args) {
  auto client = (UserClient*)clientbase;

  const auto& paused = args.at(1);
  auto& paused_map = paused.object();
  for (auto& it : paused_map) {
    auto sub_iter = client->subscriptions.find(it.first);
    if (sub_iter == client->subscriptions.end()) {
      client->sendErrorResponse(
          "this client does not have a subscription named '{}'", it.first);
      return;
    }
    if (!it.second.isBool()) {
      client->sendErrorResponse(
          "new value for subscription '{}' not a boolean", it.first);
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
  clientbase->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-set-subscriptions-paused",
    cmd_debug_set_subscriptions_paused,
    CMD_DAEMON,
    nullptr)

static json_ref getDebugSubscriptionInfo(Root* root) {
  auto subscriptions = json_array();
  for (const auto& user_client : UserClient::getAllClients()) {
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
    Client* clientbase,
    const json_ref& args) {
  auto client = (UserClient*)clientbase;

  auto root = resolveRoot(client, args);

  auto resp = make_response();
  auto debug_info = root->unilateralResponses->getDebugInfo();
  // copy over all the key-value pairs from debug_info
  resp.object().insert(debug_info.object().begin(), debug_info.object().end());

  auto subscriptions = getDebugSubscriptionInfo(root.get());
  resp.object().emplace("subscriptions", subscriptions);

  clientbase->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-get-subscriptions",
    cmd_debug_get_subscriptions,
    CMD_DAEMON,
    w_cmd_realpath_root)

static void cmd_debug_get_asserted_states(
    Client* clientbase,
    const json_ref& args) {
  auto client = (UserClient*)clientbase;

  auto root = resolveRoot(client, args);
  auto response = make_response();

  // copy over all the key-value pairs to stateSet and release lock
  auto states = root->assertedStates.rlock()->debugStates();
  response.set(
      {{"root", w_string_to_json(root->root_path)},
       {"states", std::move(states)}});
  clientbase->enqueueResponse(std::move(response));
}
W_CMD_REG(
    "debug-get-asserted-states",
    cmd_debug_get_asserted_states,
    CMD_DAEMON,
    w_cmd_realpath_root)

static void cmd_debug_status(Client* client, const json_ref&) {
  auto resp = make_response();
  auto roots = Root::getStatusForAllRoots();
  resp.set("roots", std::move(roots));
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-status",
    cmd_debug_status,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    NULL)

static void cmd_debug_watcher_info(Client* clientbase, const json_ref& args) {
  auto* client = static_cast<UserClient*>(clientbase);

  auto root = resolveRoot(client, args);
  auto response = make_response();
  response.set("watcher-debug-info", root->view()->getWatcherDebugInfo());
  client->enqueueResponse(std::move(response));
}
W_CMD_REG("debug-watcher-info", cmd_debug_watcher_info, CMD_DAEMON, NULL)

static void cmd_debug_watcher_info_clear(
    Client* clientbase,
    const json_ref& args) {
  auto* client = static_cast<UserClient*>(clientbase);

  auto root = resolveRoot(client, args);
  auto response = make_response();
  root->view()->clearWatcherDebugInfo();
  client->enqueueResponse(std::move(response));
}
W_CMD_REG(
    "debug-watcher-info-clear",
    cmd_debug_watcher_info_clear,
    CMD_DAEMON,
    NULL)

namespace {

void addCacheStats(json_ref& resp, const CacheStats& stats) {
  resp.set(
      {{"cacheHit", json_integer(stats.cacheHit)},
       {"cacheShare", json_integer(stats.cacheShare)},
       {"cacheMiss", json_integer(stats.cacheMiss)},
       {"cacheEvict", json_integer(stats.cacheEvict)},
       {"cacheStore", json_integer(stats.cacheStore)},
       {"cacheLoad", json_integer(stats.cacheLoad)},
       {"cacheErase", json_integer(stats.cacheErase)},
       {"clearCount", json_integer(stats.clearCount)},
       {"size", json_integer(stats.size)}});
}

void debugContentHashCache(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    client->sendErrorResponse(
        "wrong number of arguments for 'debug-contenthash'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto view = std::dynamic_pointer_cast<InMemoryView>(root->view());
  if (!view) {
    client->sendErrorResponse("root is not an InMemoryView watcher");
    return;
  }

  auto stats = view->debugAccessCaches().contentHashCache.stats();
  auto resp = make_response();
  addCacheStats(resp, stats);
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-contenthash",
    debugContentHashCache,
    CMD_DAEMON,
    w_cmd_realpath_root)

void debugSymlinkTargetCache(Client* client, const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    client->sendErrorResponse(
        "wrong number of arguments for 'debug-symlink-target-cache'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto view = std::dynamic_pointer_cast<InMemoryView>(root->view());
  if (!view) {
    client->sendErrorResponse("root is not an InMemoryView watcher");
    return;
  }

  auto stats = view->debugAccessCaches().symlinkTargetCache.stats();
  auto resp = make_response();
  addCacheStats(resp, stats);
  client->enqueueResponse(std::move(resp));
}
W_CMD_REG(
    "debug-symlink-target-cache",
    debugSymlinkTargetCache,
    CMD_DAEMON,
    w_cmd_realpath_root)

} // namespace

/* vim:ts=2:sw=2:et:
 */
