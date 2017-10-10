/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "MapUtil.h"
#include "make_unique.h"
#include "watchman.h"

using ms = std::chrono::milliseconds;
using watchman::ClientStateAssertion;

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

static void cmd_state_enter(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);

  auto root = resolveRoot(client, args);

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  auto assertion = std::make_shared<ClientStateAssertion>(root, parsed.name);

  if (!mapInsert(*root->assertedStates.wlock(), assertion->name, assertion)) {
    send_error_response(
        client, "state %s is already asserted", parsed.name.c_str());
    return;
  }

  // Record the state assertion in the client
  client->states[parsed.name] = assertion;

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  auto response = make_response();

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
    std::shared_ptr<ClientStateAssertion> assertion,
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

  // Now remove the state assertion
  mapRemove(*assertion->root->assertedStates.wlock(), assertion->name);

  if (client) {
    mapRemove(client->states, assertion->name);
  }
}

// Abandon any states that haven't been explicitly vacated
void w_client_vacate_states(struct watchman_user_client *client) {
  while (!client->states.empty()) {
    auto it = client->states.begin();
    auto assertion = it->second.lock();

    if (!assertion) {
      client->states.erase(it->first);
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
  std::shared_ptr<ClientStateAssertion> assertion;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);

  auto root = resolveRoot(client, args);

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  // mapGetDefault will return a nullptr assertion as the default value, if
  // the entry is missing
  assertion = mapGetDefault(*root->assertedStates.rlock(), parsed.name);
  if (!assertion) {
    send_error_response(
        client, "state %s is not asserted", parsed.name.c_str());
    return;
  }

  // Sanity check ownership
  if (mapGetDefault(client->states, parsed.name).lock() != assertion) {
    send_error_response(
        client,
        "state %s was not asserted by this session",
        parsed.name.c_str());
    return;
  }

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  auto response = make_response();
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
