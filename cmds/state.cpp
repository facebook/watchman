/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct state_arg {
  w_string name;
  int sync_timeout;
  json_ref metadata;
};

// Parses the args for state-enter and state-leave
static bool parse_state_arg(
    struct watchman_client* client,
    const json_ref& args,
    struct state_arg* parsed) {
  parsed->sync_timeout = DEFAULT_QUERY_SYNC_MS;
  parsed->metadata = nullptr;
  parsed->name = nullptr;

  if (json_array_size(args) != 3) {
    send_error_response(client,
        "invalid number of arguments, expected 3, got %d",
        json_array_size(args));
    return false;
  }

  const auto& state_args = args.at(2);

  // [cmd, root, statename]
  if (json_is_string(state_args)) {
    parsed->name = json_to_w_string(state_args);
    return true;
  }

  // [cmd, root, {name:, metadata:, sync_timeout:}]
  parsed->name = json_to_w_string(state_args.get("name"));
  parsed->metadata = state_args.get_default("metadata");
  parsed->sync_timeout = json_integer_value(state_args.get_default(
      "sync_timeout", json_integer(parsed->sync_timeout)));

  if (parsed->sync_timeout < 0) {
    send_error_response(client, "sync_timeout must be >= 0");
    return false;
  }

  return true;
}

watchman_client_state_assertion::watchman_client_state_assertion(
    w_root_t* root,
    const w_string& name)
    : root(root), name(name), id(0) {
  w_root_addref(root);
}

watchman_client_state_assertion::~watchman_client_state_assertion() {
  w_root_delref_raw(root);
}

static void cmd_state_enter(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed = {nullptr, 0, nullptr};
  std::unique_ptr<watchman_client_state_assertion> assertion;
  char clockbuf[128];
  json_ref response;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout &&
      !w_root_sync_to_now(&unlocked, parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    goto done;
  }

  assertion.reset(
      new watchman_client_state_assertion(unlocked.root, parsed.name));
  if (!assertion) {
    send_error_response(client, "out of memory");
    goto done;
  }

  {
    auto wlock = unlocked.root->asserted_states.wlock();
    auto& map = *wlock;
    auto& entry = map[assertion->name];

    // If the state is already asserted, we can't re-assert it
    if (entry) {
      send_error_response(
          client, "state %s is already asserted", parsed.name.c_str());
      goto done;
    }
    // We're now in this state
    entry = std::move(assertion);

    // Record the state assertion in the client
    entry->id = ++client->next_state_id;
    client->states[entry->id] = entry.get();
  }

  w_root_read_lock(&unlocked, "state-enter", &lock);
  {
    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(
        lock.root->inner.number,
        lock.root->inner.ticks,
        clockbuf,
        sizeof(clockbuf));
  }
  w_root_read_unlock(&lock, &unlocked);

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  response.set({{"root", w_string_to_json(unlocked.root->root_path)},
                {"state-enter", w_string_to_json(parsed.name)},
                {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)}});
  send_and_dispose_response(client, std::move(response));

  // Now find all the clients with subscriptions and send them
  // notice of the state being entered
  {
    auto clientsLock = clients.wlock();
    for (auto subclient_base : *clientsLock) {
      auto subclient = dynamic_cast<watchman_user_client*>(subclient_base);

      for (auto& citer : subclient->subscriptions) {
        auto sub = citer.second.get();

        if (sub->root != unlocked.root) {
          w_log(W_LOG_DBG, "root doesn't match, skipping\n");
          continue;
        }

        auto pdu = make_response();
        pdu.set({{"root", w_string_to_json(unlocked.root->root_path)},
                 {"subscription", w_string_to_json(sub->name)},
                 {"unilateral", json_true()},
                 {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)},
                 {"state-enter", w_string_to_json(parsed.name)}});
        if (parsed.metadata) {
          pdu.set("metadata", json_ref(parsed.metadata));
        }
        enqueue_response(subclient, std::move(pdu), true);
      }
    }
  }

done:
  w_root_delref(&unlocked);
}
W_CMD_REG("state-enter", cmd_state_enter, CMD_DAEMON, w_cmd_realpath_root)

static void leave_state(struct watchman_user_client *client,
    struct watchman_client_state_assertion *assertion,
    bool abandoned, json_t *metadata, const char *clockbuf) {
  char buf[128];
  struct unlocked_watchman_root unlocked = {assertion->root};

  if (!clockbuf) {
    struct read_locked_watchman_root lock;
    w_root_read_lock(&unlocked, "state-leave", &lock);
    clock_id_string(
        lock.root->inner.number, lock.root->inner.ticks, buf, sizeof(buf));
    w_root_read_unlock(&lock, &unlocked);

    clockbuf = buf;
  }

  // First locate all subscribers and notify them
  {
    auto clientsLock = clients.wlock();
    for (auto subclient_base : *clientsLock) {
      auto subclient = dynamic_cast<watchman_user_client*>(subclient_base);

      for (auto& citer : subclient->subscriptions) {
        auto sub = citer.second.get();

        if (sub->root != unlocked.root) {
          w_log(W_LOG_DBG, "root doesn't match, skipping\n");
          continue;
        }

        auto pdu = make_response();
        pdu.set({{"root", w_string_to_json(unlocked.root->root_path)},
                 {"subscription", w_string_to_json(sub->name)},
                 {"unilateral", json_true()},
                 {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)},
                 {"state-leave", w_string_to_json(assertion->name)}});
        if (metadata) {
          pdu.set("metadata", json_ref(metadata));
        }
        if (abandoned) {
          pdu.set("abandoned", json_true());
        }
        enqueue_response(subclient, std::move(pdu), true);
      }
    }
  }

  // The erase will delete the assertion pointer, so save these things
  auto id = assertion->id;
  w_string name = assertion->name;

  // Now remove the state
  {
    auto map = unlocked.root->asserted_states.wlock();
    map->erase(name);
  }

  if (client) {
    client->states.erase(id);
  }
}

// Abandon any states that haven't been explicitly vacated
void w_client_vacate_states(struct watchman_user_client *client) {
  while (!client->states.empty()) {
    auto assertion = client->states.begin()->second;
    auto root = assertion->root;

    w_log(
        W_LOG_ERR,
        "implicitly vacating state %s on %s due to client disconnect\n",
        assertion->name.c_str(),
        root->root_path.c_str());

    // This will delete the state from client->states and invalidate
    // the iterator.
    leave_state(client, assertion, true, NULL, NULL);
  }
}

static void cmd_state_leave(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed = {nullptr, 0, nullptr};
  // This is a weak reference to the assertion.  This is safe because only this
  // client can delete this assertion, and this function is only executed by
  // the thread that owns this client.
  struct watchman_client_state_assertion *assertion = NULL;
  char clockbuf[128];
  auto client = dynamic_cast<watchman_user_client*>(clientbase);
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;
  json_ref response;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout &&
      !w_root_sync_to_now(&unlocked, parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    goto done;
  }

  {
    auto map = unlocked.root->asserted_states.rlock();
    // Confirm that this client owns this state
    const auto& it = map->find(parsed.name);
    // If the state is not asserted, we can't leave it
    if (it == map->end()) {
      send_error_response(
          client, "state %s is not asserted", parsed.name.c_str());
      goto done;
    }

    assertion = it->second.get();

    // Sanity check ownership
    if (client->states[assertion->id] != assertion) {
      send_error_response(
          client,
          "state %s was not asserted by this session",
          parsed.name.c_str());
      goto done;
    }
  }

  w_root_read_lock(&unlocked, "state-leave", &lock);
  {
    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(
        lock.root->inner.number,
        lock.root->inner.ticks,
        clockbuf,
        sizeof(clockbuf));
  }
  w_root_read_unlock(&lock, &unlocked);

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  response.set({{"root", w_string_to_json(unlocked.root->root_path)},
                {"state-leave", w_string_to_json(parsed.name)},
                {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)}});
  send_and_dispose_response(client, std::move(response));

  // Notify and exit the state
  leave_state(client, assertion, false, parsed.metadata, clockbuf);

done:
  w_root_delref(&unlocked);
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
