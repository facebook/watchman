/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "make_unique.h"

static void w_run_subscription_rules(
    struct watchman_user_client* client,
    struct watchman_client_subscription* sub,
    struct read_locked_watchman_root* lock);

/** This is called from the IO thread */
void process_subscriptions(struct read_locked_watchman_root* lock) {
  bool vcs_in_progress;
  const w_root_t* root = lock->root;

  auto clientsLock = clients.wlock();

  if (clientsLock->empty()) {
    // No subscribers
    return;
  }

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer subscription
  // notifications until things settle down
  vcs_in_progress = lock->root->inner.view->isVCSOperationInProgress();

  for (auto client_base : *clientsLock) {
    auto client = (watchman_user_client*)client_base;
    for (auto& citer : client->subscriptions) {
      auto sub = citer.second.get();
      bool defer = false;
      bool drop = false;
      w_string policy_name;

      if (sub->root != root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }
      w_log(
          W_LOG_DBG,
          "client->stm=%p sub=%p %s, last=%" PRIu32 " pending=%" PRIu32 "\n",
          client->stm,
          sub,
          sub->name.c_str(),
          sub->last_sub_tick,
          root->inner.view->getMostRecentTickValue());

      if (sub->last_sub_tick == root->inner.view->getMostRecentTickValue()) {
        continue;
      }

      {
        auto asserted_states = root->asserted_states.rlock();
        if (!asserted_states->empty() && !sub->drop_or_defer.empty()) {
          policy_name.reset();

          // There are 1 or more states asserted and this subscription
          // has some policy for states.  Figure out what we should do.
          for (auto& policy_iter : sub->drop_or_defer) {
            auto name = policy_iter.first;
            bool policy_is_drop = policy_iter.second;

            if (asserted_states->find(name) == asserted_states->end()) {
              continue;
            }

            if (!defer) {
              // This policy is active
              defer = true;
              policy_name = name;
            }

            if (policy_is_drop) {
              drop = true;

              // If we're dropping, we don't need to look at any
              // other policies
              policy_name = name;
              break;
            }
            // Otherwise keep looking until we find a drop
          }
        }
      }

      if (drop) {
        // fast-forward over any notifications while in the drop state
        sub->last_sub_tick = root->inner.view->getMostRecentTickValue();
        w_log(
            W_LOG_DBG,
            "dropping subscription notifications for %s "
            "until state %s is vacated\n",
            sub->name.c_str(),
            policy_name.c_str());
        continue;
      }

      if (defer) {
        w_log(
            W_LOG_DBG,
            "deferring subscription notifications for %s "
            "until state %s is vacated\n",
            sub->name.c_str(),
            policy_name.c_str());
        continue;
      }

      if (sub->vcs_defer && vcs_in_progress) {
        w_log(
            W_LOG_DBG,
            "deferring subscription notifications for %s "
            "until VCS operations complete\n",
            sub->name.c_str());
        continue;
      }

      w_run_subscription_rules(client, sub, lock);
      sub->last_sub_tick = root->inner.view->getMostRecentTickValue();
    }
  }
}

static void update_subscription_ticks(struct watchman_client_subscription *sub,
    w_query_res *res) {
  // create a new spec that will be used the next time
  sub->query->since_spec = w_clockspec_new_clock(res->root_number, res->ticks);
}

static json_ref build_subscription_results(
    struct watchman_client_subscription* sub,
    struct read_locked_watchman_root* lock) {
  w_query_res res;
  char clockbuf[128];
  auto since_spec = sub->query->since_spec.get();

  if (since_spec && since_spec->tag == w_cs_clock) {
    w_log(
        W_LOG_DBG,
        "running subscription %s rules since %" PRIu32 "\n",
        sub->name.c_str(),
        since_spec->clock.ticks);
  } else {
    w_log(
        W_LOG_DBG,
        "running subscription %s rules (no since)\n",
        sub->name.c_str());
  }

  // Subscriptions never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  sub->query->sync_timeout = 0;
  // We're called by the io thread, so there's little chance that the root
  // could be legitimately blocked by something else.  That means that we
  // can use a short lock_timeout
  sub->query->lock_timeout =
      uint32_t(lock->root->config.getInt("subscription_lock_timeout_ms", 100));
  w_log(W_LOG_DBG, "running subscription %s %p\n", sub->name.c_str(), sub);

  if (!w_query_execute_locked(sub->query.get(), lock, &res, time_generator)) {
    w_log(
        W_LOG_ERR,
        "error running subscription %s query: %s",
        sub->name.c_str(),
        res.errmsg);
    return nullptr;
  }

  w_log(
      W_LOG_DBG,
      "subscription %s generated %" PRIu32 " results\n",
      sub->name.c_str(),
      uint32_t(res.results.size()));

  if (res.results.empty()) {
    update_subscription_ticks(sub, &res);
    return nullptr;
  }

  auto file_list = w_query_results_to_json(
      &sub->field_list, res.results.size(), res.results);

  auto response = make_response();

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock &&
      clock_id_string(since_spec->clock.root_number, since_spec->clock.ticks,
                      clockbuf, sizeof(clockbuf))) {
    response.set("since", typed_string_to_json(clockbuf, W_STRING_UNICODE));
  }
  if (clock_id_string(res.root_number, res.ticks, clockbuf, sizeof(clockbuf))) {
    response.set("clock", typed_string_to_json(clockbuf, W_STRING_UNICODE));
  }
  update_subscription_ticks(sub, &res);

  response.set({{"is_fresh_instance", json_boolean(res.is_fresh_instance)},
                {"files", std::move(file_list)},
                {"root", w_string_to_json(lock->root->root_path)},
                {"subscription", w_string_to_json(sub->name)},
                {"unilateral", json_true()}});

  return response;
}

static void w_run_subscription_rules(
    struct watchman_user_client* client,
    struct watchman_client_subscription* sub,
    struct read_locked_watchman_root* lock) {
  auto response = build_subscription_results(sub, lock);

  if (!response) {
    return;
  }

  add_root_warnings_to_response(response, lock);

  if (!enqueue_response(client, std::move(response), true)) {
    w_log(W_LOG_DBG, "failed to queue sub response\n");
  }
}

void w_cancel_subscriptions_for_root(const w_root_t *root) {
  auto lock = clients.wlock();
  for (auto client_base : *lock) {
    auto client = (watchman_user_client*)client_base;
    // Manually iterate since we will be erasing elements as we go
    auto citer = client->subscriptions.begin();
    while (citer != client->subscriptions.end()) {
      auto sub = citer->second.get();

      if (sub->root == root) {
        auto response = make_response();

        w_log(
            W_LOG_ERR,
            "Cancel subscription %s for client:stm=%p due to "
            "root cancellation\n",
            sub->name.c_str(),
            client->stm);

        response.set({{"root", w_string_to_json(root->root_path)},
                      {"subscription", w_string_to_json(sub->name)},
                      {"unilateral", json_true()},
                      {"canceled", json_true()}});

        if (!enqueue_response(client, std::move(response), true)) {
          w_log(W_LOG_DBG, "failed to queue sub cancellation\n");
        }

        citer = client->subscriptions.erase(citer);
      } else {
        ++citer;
      }
    }
  }
}

/* unsubscribe /root subname
 * Cancels a subscription */
static void cmd_unsubscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  const char *name;
  bool deleted{false};
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  auto jstr = json_array_get(args, 2);
  name = json_string_value(jstr);
  if (!name) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    w_root_delref(&unlocked);
    return;
  }

  auto sname = json_to_w_string(jstr);

  {
    auto lock = clients.wlock();
    auto it = client->subscriptions.find(sname);
    if (it != client->subscriptions.end()) {
      client->subscriptions.erase(it);
      deleted = true;
    }
  }

  auto resp = make_response();
  resp.set({{"unsubscribe", typed_string_to_json(name)},
            {"deleted", json_boolean(deleted)}});

  send_and_dispose_response(client, std::move(resp));
  w_root_delref(&unlocked);
}
W_CMD_REG("unsubscribe", cmd_unsubscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
static void cmd_subscribe(
    struct watchman_client* clientbase,
    const json_ref& args) {
  std::unique_ptr<watchman_client_subscription> sub;
  watchman_client_subscription* subPtr{nullptr};
  json_ref resp, initial_subscription_results;
  json_ref jfield_list;
  json_ref jname;
  std::shared_ptr<w_query> query;
  json_ref query_spec;
  struct w_query_field_list field_list;
  char *errmsg;
  int defer = true; /* can't use bool because json_unpack requires int */
  json_ref defer_list;
  json_ref drop_list;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct unlocked_watchman_root unlocked;
  struct read_locked_watchman_root lock;

  if (json_array_size(args) != 4) {
    send_error_response(client, "wrong number of arguments for subscribe");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  jname = args.at(2);
  if (!json_is_string(jname)) {
    send_error_response(
        client, "expected 2nd parameter to be subscription name");
    goto done;
  }

  query_spec = args.at(3);

  jfield_list = query_spec.get_default("fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(client, "invalid field list: %s", errmsg);
    free(errmsg);
    goto done;
  }

  query = w_query_parse(unlocked.root, query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    goto done;
  }

  defer_list = query_spec.get_default("defer");
  if (defer_list && !json_is_array(defer_list)) {
    send_error_response(client, "defer field must be an array of strings");
    goto done;
  }

  drop_list = query_spec.get_default("drop");
  if (drop_list && !json_is_array(drop_list)) {
    send_error_response(client, "drop field must be an array of strings");
    goto done;
  }

  sub = watchman::make_unique<watchman_client_subscription>();
  if (!sub) {
    send_error_response(client, "no memory!");
    goto done;
  }

  sub->name = json_to_w_string(jname);
  sub->query = query;

  json_unpack(query_spec, "{s?:b}", "defer_vcs", &defer);
  sub->vcs_defer = defer;

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

  memcpy(&sub->field_list, &field_list, sizeof(field_list));
  sub->root = unlocked.root;

  // This locking is to ensure that we publish a subscription when
  // it is fully constructed.  No other threads can delete the subscription;
  // it is owned by the client thread which is calling this function right
  // now, so it is safe to hold on to the raw pointer while we do the tail
  // processing in the remainder of the function below.
  subPtr = sub.get();
  {
    auto lock = clients.wlock();
    client->subscriptions[subPtr->name] = std::move(sub);
  }

  resp = make_response();
  resp.set("subscribe", json_ref(jname));

  w_root_read_lock(&unlocked, "initial subscription query", &lock);

  add_root_warnings_to_response(resp, &lock);
  annotate_with_clock(&lock, resp);
  initial_subscription_results = build_subscription_results(subPtr, &lock);
  w_root_read_unlock(&lock, &unlocked);

  send_and_dispose_response(client, std::move(resp));
  if (initial_subscription_results) {
    send_and_dispose_response(client, std::move(initial_subscription_results));
  }
done:
  w_root_delref(&unlocked);
}
W_CMD_REG("subscribe", cmd_subscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
