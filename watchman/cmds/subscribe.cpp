/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/MapUtil.h"
#include "watchman/watchman.h"

using namespace watchman;

watchman_client_subscription::watchman_client_subscription(
    const std::shared_ptr<watchman_root>& root,
    std::weak_ptr<watchman_client> client)
    : root(root), weakClient(client) {}

std::shared_ptr<watchman_user_client>
watchman_client_subscription::lockClient() {
  auto client = weakClient.lock();
  if (client) {
    return std::dynamic_pointer_cast<watchman_user_client>(client);
  }
  return nullptr;
}

watchman_client_subscription::~watchman_client_subscription() {
  auto client = lockClient();
  if (client) {
    client->unsubByName(name);
  }
}

bool watchman_user_client::unsubByName(const w_string& name) {
  auto subIter = subscriptions.find(name);
  if (subIter == subscriptions.end()) {
    return false;
  }

  // Break the weakClient pointer so that ~watchman_client_subscription()
  // cannot successfully lockClient and recursively call us.
  subIter->second->weakClient.reset();
  unilateralSub.erase(subIter->second);
  subscriptions.erase(subIter);

  return true;
}

enum class sub_action { no_sync_needed, execute, defer, drop };

static std::tuple<sub_action, w_string> get_subscription_action(
    struct watchman_client_subscription* sub,
    const std::shared_ptr<watchman_root>& root,
    ClockPosition position) {
  auto action = sub_action::execute;
  w_string policy_name;

  watchman::log(
      watchman::DBG,
      "sub=",
      fmt::ptr(sub),
      " ",
      sub->name,
      ", last=",
      sub->last_sub_tick,
      " pending=",
      position.ticks,
      "\n");

  if (sub->last_sub_tick != position.ticks) {
    if (!sub->drop_or_defer.empty()) {
      auto assertedStates = root->assertedStates.rlock();
      // This subscription has some policy for states.
      // Figure out what we should do.
      for (auto& policy_iter : sub->drop_or_defer) {
        auto name = policy_iter.first;
        bool policy_is_drop = policy_iter.second;

        if (!assertedStates->isStateAsserted(name)) {
          continue;
        }

        if (action != sub_action::defer) {
          // This policy is active
          action = sub_action::defer;
          policy_name = name;
        }

        if (policy_is_drop) {
          action = sub_action::drop;

          // If we're dropping, we don't need to look at any
          // other policies
          policy_name = name;
          break;
        }
        // Otherwise keep looking until we find a drop
      }
    }
  } else {
    watchman::log(
        watchman::DBG, "subscription ", sub->name, " is up to date\n");
    action = sub_action::no_sync_needed;
  }

  return std::make_tuple(action, policy_name);
}

void watchman_client_subscription::processSubscription() {
  try {
    processSubscriptionImpl();
  } catch (const std::system_error& exc) {
    if (exc.code() == error_code::stale_file_handle) {
      // This can happen if, for example, the Eden filesystem got into
      // a weird state without fully unmounting from the VFS.
      // In this situation we're getting a signal that the root is no longer
      // valid so the correct action is to cancel the watch.
      log(ERR,
          "While processing subscriptions for ",
          root->root_path,
          " got: ",
          exc.what(),
          ".  Cancel watch\n");
      root->cancel();
    } else {
      throw;
    }
  }
}

void watchman_client_subscription::processSubscriptionImpl() {
  auto client = lockClient();
  if (!client) {
    watchman::log(
        watchman::ERR,
        "encountered a vacated client while running subscription rules\n");
    return;
  }

  sub_action action;
  w_string policy_name;
  auto position = root->view()->getMostRecentRootNumberAndTickValue();
  std::tie(action, policy_name) = get_subscription_action(this, root, position);

  if (action != sub_action::no_sync_needed) {
    bool executeQuery = true;

    if (action == sub_action::drop) {
      // fast-forward over any notifications while in the drop state
      last_sub_tick = position.ticks;
      query->since_spec = std::make_unique<ClockSpec>(position);
      watchman::log(
          watchman::DBG,
          "dropping subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated. Advanced ticks to ",
          last_sub_tick,
          "\n");
      executeQuery = false;
    } else if (action == sub_action::defer) {
      watchman::log(
          watchman::DBG,
          "deferring subscription notifications for ",
          name,
          " until state ",
          policy_name,
          " is vacated\n");
      executeQuery = false;
    } else if (vcs_defer && root->view()->isVCSOperationInProgress()) {
      watchman::log(
          watchman::DBG,
          "deferring subscription notifications for ",
          name,
          " until VCS operations complete\n");
      executeQuery = false;
    }

    if (executeQuery) {
      try {
        last_sub_tick =
            runSubscriptionRules(client.get(), root).position().ticks;
      } catch (const std::exception& exc) {
        // This may happen if an SCM aware query fails to run hg for
        // whatever reason.  Since last_sub_tick is not advanced,
        // we haven't missed any results and will re-evaluate with
        // the same basis the next time a file is changed.  Due to
        // the way that hg works, it is quite likely that it has
        // touched some files already and that we'll get called
        // again almost immediately.
        log(ERR,
            "Error while performing query for subscription ",
            name,
            ": ",
            exc.what(),
            ". Deferring until next change.\n");
      }
    }
  } else {
    watchman::log(watchman::DBG, "subscription ", name, " is up to date\n");
  }
}

void watchman_client_subscription::updateSubscriptionTicks(w_query_res* res) {
  // create a new spec that will be used the next time
  query->since_spec = std::make_unique<ClockSpec>(res->clockAtStartOfQuery);
}

json_ref watchman_client_subscription::buildSubscriptionResults(
    const std::shared_ptr<watchman_root>& root,
    ClockSpec& position,
    OnStateTransition onStateTransition) {
  auto since_spec = query->since_spec.get();

  if (since_spec && since_spec->tag == w_cs_clock) {
    watchman::log(
        watchman::DBG,
        "running subscription ",
        name,
        " rules since ",
        since_spec->clock.position.ticks,
        "\n");
  } else {
    watchman::log(
        watchman::DBG, "running subscription ", name, " rules (no since)\n");
  }

  // Subscriptions never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  query->sync_timeout = std::chrono::milliseconds(0);
  // We're called by the io thread, so there's little chance that the root
  // could be legitimately blocked by something else.  That means that we
  // can use a short lock_timeout
  query->lock_timeout =
      uint32_t(root->config.getInt("subscription_lock_timeout_ms", 100));
  logf(DBG, "running subscription {} {}\n", name, fmt::ptr(this));

  try {
    auto res = w_query_execute(query.get(), root, time_generator);

    logf(
        DBG,
        "subscription {} generated {} results\n",
        name,
        res.resultsArray.array().size());

    position = res.clockAtStartOfQuery;

    // An SCM operation was interleaved with the query execution. This could
    // result in over-reporing query results. Discard our results but, do not
    // update the clock in order to allow changes to be reported the next time
    // the query is run.
    bool scmAwareQuery = since_spec && since_spec->hasScmParams();
    if (onStateTransition == OnStateTransition::DontAdvance && scmAwareQuery) {
      if (root->stateTransCount.load() != res.stateTransCountAtStartOfQuery) {
        watchman::log(
            watchman::DBG,
            "discarding SCM aware query results, SCM activity interleaved\n");
        return nullptr;
      }
    }

    // We can suppress empty results, unless this is a source code aware query
    // and the mergeBase has changed or this is a fresh instance.
    bool mergeBaseChanged = scmAwareQuery &&
        res.clockAtStartOfQuery.scmMergeBase != query->since_spec->scmMergeBase;
    if (res.resultsArray.array().empty() && !mergeBaseChanged &&
        !res.is_fresh_instance) {
      updateSubscriptionTicks(&res);
      return nullptr;
    }

    auto response = make_response();

    // It is way too much of a hassle to try to recreate the clock value if it's
    // not a relative clock spec, and it's only going to happen on the first run
    // anyway, so just skip doing that entirely.
    if (since_spec && since_spec->tag == w_cs_clock) {
      response.set("since", since_spec->toJson());
    }
    updateSubscriptionTicks(&res);

    response.set(
        {{"is_fresh_instance", json_boolean(res.is_fresh_instance)},
         {"clock", res.clockAtStartOfQuery.toJson()},
         {"files", std::move(res.resultsArray)},
         {"root", w_string_to_json(root->root_path)},
         {"subscription", w_string_to_json(name)},
         {"unilateral", json_true()}});
    if (res.savedStateInfo) {
      response.set({{"saved-state-info", std::move(res.savedStateInfo)}});
    }

    return response;
  } catch (const QueryExecError& e) {
    watchman::log(
        watchman::ERR,
        "error running subscription ",
        name,
        " query: ",
        e.what());
    return nullptr;
  }
}

ClockSpec watchman_client_subscription::runSubscriptionRules(
    watchman_user_client* client,
    const std::shared_ptr<watchman_root>& root) {
  ClockSpec position;

  auto response =
      buildSubscriptionResults(root, position, OnStateTransition::DontAdvance);

  if (response) {
    add_root_warnings_to_response(response, root);
    client->enqueueResponse(std::move(response), false);
  }
  return position;
}

static void cmd_flush_subscriptions(
    struct watchman_client* clientbase,
    const json_ref& args) {
  auto client = (watchman_user_client*)clientbase;

  int sync_timeout;
  json_ref subs(nullptr);

  if (json_array_size(args) == 3) {
    auto& sync_timeout_obj = args.at(2).get("sync_timeout");
    subs = args.at(2).get_default("subscriptions", nullptr);
    if (!sync_timeout_obj.isInt()) {
      send_error_response(client, "'sync_timeout' must be an integer");
      return;
    }
    sync_timeout = sync_timeout_obj.asInt();
  } else {
    send_error_response(
        client, "wrong number of arguments to 'flush-subscriptions'");
    return;
  }

  auto root = resolveRoot(client, args);

  std::vector<w_string> subs_to_sync;
  if (subs) {
    if (!subs.isArray()) {
      send_error_response(
          client,
          "expected 'subscriptions' to be an array of subscription names");
      return;
    }

    for (auto& sub_name : subs.array()) {
      if (!sub_name.isString()) {
        send_error_response(
            client,
            "expected 'subscriptions' to be an array of subscription names");
        return;
      }

      auto& sub_name_str = json_to_w_string(sub_name);
      auto sub_iter = client->subscriptions.find(sub_name_str);
      if (sub_iter == client->subscriptions.end()) {
        send_error_response(
            client,
            "this client does not have a subscription named '%s'",
            sub_name_str.c_str());
        return;
      }
      auto& sub = sub_iter->second;
      if (sub->root != root) {
        send_error_response(
            client,
            "subscription '%s' is on root '%s' different from command root "
            "'%s'",
            sub_name_str.c_str(),
            sub->root->root_path.c_str(),
            root->root_path.c_str());
        return;
      }

      subs_to_sync.push_back(sub_name_str);
    }
  } else {
    // Look for all subscriptions matching this root.
    for (auto& sub_iter : client->subscriptions) {
      if (sub_iter.second->root == root) {
        subs_to_sync.push_back(sub_iter.first);
      }
    }
  }

  root->syncToNow(std::chrono::milliseconds(sync_timeout));

  auto resp = make_response();
  auto synced = json_array();
  auto no_sync_needed = json_array();
  auto dropped = json_array();

  for (auto& sub_name_str : subs_to_sync) {
    auto sub_iter = client->subscriptions.find(sub_name_str);
    auto& sub = sub_iter->second;

    sub_action action;
    w_string policy_name;
    auto position = root->view()->getMostRecentRootNumberAndTickValue();
    std::tie(action, policy_name) =
        get_subscription_action(sub.get(), root, position);

    if (action == sub_action::drop) {
      sub->last_sub_tick = position.ticks;
      sub->query->since_spec = std::make_unique<ClockSpec>(position);
      watchman::log(
          watchman::DBG,
          "(flush-subscriptions) dropping subscription notifications for ",
          sub->name,
          " until state ",
          policy_name,
          " is vacated. Advanced ticks to ",
          sub->last_sub_tick,
          "\n");
      json_array_append(dropped, w_string_to_json(sub_name_str));
    } else {
      // flush-subscriptions means that we _should NOT defer_ notifications. So
      // ignore defer and defer_vcs.
      ClockSpec out_position;
      watchman::log(
          watchman::DBG,
          "(flush-subscriptions) executing subscription ",
          sub->name,
          "\n");
      auto sub_result = sub->buildSubscriptionResults(
          root, out_position, OnStateTransition::QueryAnyway);
      if (sub_result) {
        send_and_dispose_response(client, std::move(sub_result));
        json_array_append(synced, w_string_to_json(sub_name_str));
      } else {
        json_array_append(no_sync_needed, w_string_to_json(sub_name_str));
      }
    }
  }

  resp.set(
      {{"synced", std::move(synced)},
       {"no_sync_needed", std::move(no_sync_needed)},
       {"dropped", std::move(dropped)}});
  add_root_warnings_to_response(resp, root);
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "flush-subscriptions",
    cmd_flush_subscriptions,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* unsubscribe /root subname
 * Cancels a subscription */
static void cmd_unsubscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  const char* name;
  bool deleted{false};
  struct watchman_user_client* client =
      (struct watchman_user_client*)clientbase;

  auto root = resolveRoot(client, args);

  auto jstr = json_array_get(args, 2);
  name = json_string_value(jstr);
  if (!name) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    return;
  }

  auto sname = json_to_w_string(jstr);
  deleted = client->unsubByName(sname);

  auto resp = make_response();
  resp.set(
      {{"unsubscribe", typed_string_to_json(name)},
       {"deleted", json_boolean(deleted)}});

  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "unsubscribe",
    cmd_unsubscribe,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
static void cmd_subscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  std::shared_ptr<watchman_client_subscription> sub;
  json_ref resp, initial_subscription_results;
  json_ref jfield_list;
  json_ref jname;
  std::shared_ptr<w_query> query;
  json_ref query_spec;
  json_ref defer_list;
  json_ref drop_list;
  struct watchman_user_client* client =
      (struct watchman_user_client*)clientbase;

  if (json_array_size(args) != 4) {
    send_error_response(client, "wrong number of arguments for subscribe");
    return;
  }

  auto root = resolveRoot(client, args);

  jname = args.at(2);
  if (!jname.isString()) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    return;
  }

  query_spec = args.at(3);

  query = w_query_parse(root, query_spec);
  query->clientPid = client->stm ? client->stm->getPeerProcessID() : 0;
  query->subscriptionName = json_to_w_string(jname);

  defer_list = query_spec.get_default("defer");
  if (defer_list && !defer_list.isArray()) {
    send_error_response(client, "defer field must be an array of strings");
    return;
  }

  drop_list = query_spec.get_default("drop");
  if (drop_list && !drop_list.isArray()) {
    send_error_response(client, "drop field must be an array of strings");
    return;
  }

  sub = std::make_shared<watchman_client_subscription>(
      root, client->shared_from_this());

  sub->name = json_to_w_string(jname);
  sub->query = query;

  auto defer = query_spec.get_default("defer_vcs", json_true());
  if (!defer.isBool()) {
    send_error_response(client, "defer_vcs must be boolean");
    return;
  }
  sub->vcs_defer = defer.asBool();

  if (drop_list || defer_list) {
    size_t i;

    if (defer_list) {
      for (i = 0; i < json_array_size(defer_list); i++) {
        sub->drop_or_defer[json_to_w_string(json_array_get(defer_list, i))] =
            false;
      }
    }
    if (drop_list) {
      for (i = 0; i < json_array_size(drop_list); i++) {
        sub->drop_or_defer[json_to_w_string(json_array_get(drop_list, i))] =
            true;
      }
    }
  }

  // If they want SCM aware results we should wait for SCM events to finish
  // before dispatching subscriptions
  if (query->since_spec && query->since_spec->hasScmParams()) {
    sub->vcs_defer = true;

    // If they didn't specify any drop/defer behavior, default to a reasonable
    // setting that works together with the fsmonitor extension for hg.
    if (watchman::mapContainsAny(
            sub->drop_or_defer, "hg.update", "hg.transaction")) {
      sub->drop_or_defer["hg.update"] = false; // defer
      sub->drop_or_defer["hg.transaction"] = false; // defer
    }
  }

  // Connect the root to our subscription
  {
    auto client_id = w_string::build(client->unique_id);
    auto client_stream = w_string::build(fmt::ptr(client->stm.get()));
    auto info_json = json_object(
        {{"name", w_string_to_json(sub->name)},
         {"query", sub->query->query_spec},
         {"client", w_string_to_json(client_id)},
         {"stm", w_string_to_json(client_stream)},
         {"is_owner", json_boolean(client->stm->peerIsOwner())},
         {"pid", json_integer(client->stm->getPeerProcessID())}});

    std::weak_ptr<watchman_client> clientRef(client->shared_from_this());
    client->unilateralSub.insert(std::make_pair(
        sub,
        root->unilateralResponses->subscribe(
            [clientRef, sub]() {
              auto client = clientRef.lock();
              if (client) {
                client->ping->notify();
              }
            },
            info_json)));
  }

  client->subscriptions[sub->name] = sub;

  resp = make_response();
  resp.set("subscribe", json_ref(jname));

  add_root_warnings_to_response(resp, root);
  ClockSpec position;
  initial_subscription_results = sub->buildSubscriptionResults(
      root, position, OnStateTransition::DontAdvance);
  resp.set("clock", position.toJson());
  auto saved_state_info =
      initial_subscription_results.get_default("saved-state-info");
  if (saved_state_info) {
    resp.set("saved-state-info", std::move(saved_state_info));
  }

  auto asserted_states = json_array();
  {
    auto rootAssertedStates = root->assertedStates.rlock();
    for (const auto& key : sub->drop_or_defer) {
      if (rootAssertedStates->isStateAsserted(key.first)) {
        // Not sure what to do in case of failure here. -jupi
        json_array_append(asserted_states, w_string_to_json(key.first));
      }
    }
  }
  resp.set("asserted-states", json_ref(asserted_states));

  send_and_dispose_response(client, std::move(resp));
  if (initial_subscription_results) {
    send_and_dispose_response(client, std::move(initial_subscription_results));
  }
}
W_CMD_REG(
    "subscribe",
    cmd_subscribe,
    CMD_DAEMON | CMD_ALLOW_ANY_USER,
    w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
