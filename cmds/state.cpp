/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "make_unique.h"

using ms = std::chrono::milliseconds;

struct state_arg {
  w_string name;
  ms sync_timeout;
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
    send_error_response(
        client,
        "invalid number of arguments, expected 3, got %" PRIsize_t,
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
  parsed->sync_timeout = ms(json_integer_value(state_args.get_default(
      "sync_timeout", json_integer(parsed->sync_timeout.count()))));

  if (parsed->sync_timeout < ms::zero()) {
    send_error_response(client, "sync_timeout must be >= 0");
    return false;
  }

  return true;
}

watchman_client_state_assertion::watchman_client_state_assertion(
    const std::shared_ptr<w_root_t>& root,
    const w_string& name)
    : root(root), name(name), id(0) {}

static void cmd_state_enter(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed;
  json_ref response;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);

  auto root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  if (parsed.sync_timeout.count() && !root->syncToNow(parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    return;
  }

  auto assertion =
      std::make_shared<watchman_client_state_assertion>(root, parsed.name);
  if (!assertion) {
    send_error_response(client, "out of memory");
    return;
  }

  {
    auto wlock = root->asserted_states.wlock();
    auto& map = *wlock;
    auto& entry = map[assertion->name];

    // If the state is already asserted, we can't re-assert it
    if (entry) {
      send_error_response(
          client, "state %s is already asserted", parsed.name.c_str());
      return;
    }
    // We're now in this state
    entry = assertion;

    // Record the state assertion in the client
    entry->id = ++client->next_state_id;
    client->states[entry->id] = entry;
  }

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();

  auto clock = w_string_to_json(root->view()->getCurrentClockString());

  response.set({{"root", w_string_to_json(root->root_path)},
                {"state-enter", w_string_to_json(parsed.name)},
                {"clock", json_ref(clock)}});
  send_and_dispose_response(client, std::move(response));

  // Broadcast about the state enter
  {
    auto payload =
        json_object({{"root", w_string_to_json(root->root_path)},
                     {"clock", std::move(clock)},
                     {"state-enter", w_string_to_json(parsed.name)}});
    if (parsed.metadata) {
      payload.set("metadata", json_ref(parsed.metadata));
    }
    root->unilateralResponses->enqueue(std::move(payload));
  }
}
W_CMD_REG("state-enter", cmd_state_enter, CMD_DAEMON, w_cmd_realpath_root)

static void leave_state(
    struct watchman_user_client* client,
    std::shared_ptr<watchman_client_state_assertion> assertion,
    bool abandoned,
    json_t* metadata) {
  // Broadcast about the state leave
  auto payload = json_object(
      {{"root", w_string_to_json(assertion->root->root_path)},
       {"clock",
        w_string_to_json(assertion->root->view()->getCurrentClockString())},
       {"state-leave", w_string_to_json(assertion->name)}});
  if (metadata) {
    payload.set("metadata", json_ref(metadata));
  }
  if (abandoned) {
    payload.set("abandoned", json_true());
  }
  assertion->root->unilateralResponses->enqueue(std::move(payload));

  // The erase will delete the assertion pointer, so save these things
  auto id = assertion->id;
  w_string name = assertion->name;

  // Now remove the state
  {
    auto map = assertion->root->asserted_states.wlock();
    map->erase(name);
  }

  if (client) {
    client->states.erase(id);
  }
}

// Abandon any states that haven't been explicitly vacated
void w_client_vacate_states(struct watchman_user_client *client) {
  while (!client->states.empty()) {
    auto assertion = client->states.begin()->second.lock();

    if (!assertion) {
      client->states.erase(client->states.begin()->first);
      continue;
    }

    auto root = assertion->root;

    w_log(
        W_LOG_ERR,
        "implicitly vacating state %s on %s due to client disconnect\n",
        assertion->name.c_str(),
        root->root_path.c_str());

    // This will delete the state from client->states and invalidate
    // the iterator.
    leave_state(client, assertion, true, nullptr);
  }
}

static void cmd_state_leave(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed;
  // This is a weak reference to the assertion.  This is safe because only this
  // client can delete this assertion, and this function is only executed by
  // the thread that owns this client.
  std::shared_ptr<watchman_client_state_assertion> assertion;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);
  json_ref response;

  auto root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  if (parsed.sync_timeout.count() && !root->syncToNow(parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    return;
  }

  {
    auto map = root->asserted_states.rlock();
    // Confirm that this client owns this state
    const auto& it = map->find(parsed.name);
    // If the state is not asserted, we can't leave it
    if (it == map->end()) {
      send_error_response(
          client, "state %s is not asserted", parsed.name.c_str());
      return;
    }

    assertion = it->second;

    // Sanity check ownership
    if (client->states[assertion->id].lock() != assertion) {
      send_error_response(
          client,
          "state %s was not asserted by this session",
          parsed.name.c_str());
      return;
    }
  }

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  response.set(
      {{"root", w_string_to_json(root->root_path)},
       {"state-leave", w_string_to_json(parsed.name)},
       {"clock", w_string_to_json(root->view()->getCurrentClockString())}});
  send_and_dispose_response(client, std::move(response));

  // Notify and exit the state
  leave_state(client, assertion, false, parsed.metadata);
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
