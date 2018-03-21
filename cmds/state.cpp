/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include "MapUtil.h"
#include "ThreadPool.h"
#include "make_unique.h"
#include "watchman.h"

using namespace watchman;
using ms = std::chrono::milliseconds;
using watchman::ClientStateAssertion;
using watchman::ClientStateDisposition;

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

namespace watchman {

void ClientStateAssertions::queueAssertion(
    std::shared_ptr<ClientStateAssertion> assertion) {
  // Check to see if someone else has or had a pending claim for this
  // state and reject the attempt in that case
  auto state_q = states_.find(assertion->name);
  if (state_q != states_.end() && !state_q->second.empty()) {
    auto disp = state_q->second.back()->disposition;
    if (disp == ClientStateDisposition::PendingEnter ||
        disp == ClientStateDisposition::Asserted) {
      throw std::runtime_error(to<std::string>(
          "state ", assertion->name, " is already Asserted or PendingEnter"));
    }
  }
  states_[assertion->name].push_back(assertion);
}

json_ref ClientStateAssertions::debugStates() const {
  auto states = json_array();
  for (const auto& state_q : states_) {
    for (const auto& state : state_q.second) {
      auto obj = json_object();
      obj.set("name", w_string_to_json(state->name));
      switch (state->disposition) {
        case ClientStateDisposition::PendingEnter:
          obj.set("state", w_string_to_json("PendingEnter"));
          break;
        case ClientStateDisposition::Asserted:
          obj.set("state", w_string_to_json("Asserted"));
          break;
        case ClientStateDisposition::PendingLeave:
          obj.set("state", w_string_to_json("PendingLeave"));
          break;
        case ClientStateDisposition::Done:
          obj.set("state", w_string_to_json("Done"));
          break;
      }
      json_array_append(states, obj);
    }
  }
  return states;
}

bool ClientStateAssertions::removeAssertion(
    const std::shared_ptr<ClientStateAssertion>& assertion) {
  auto it = states_.find(assertion->name);
  if (it == states_.end()) {
    return false;
  }

  auto& queue = it->second;
  for (auto assertionIter = queue.begin(); assertionIter != queue.end();
       ++assertionIter) {
    if (*assertionIter == assertion) {
      assertion->disposition = ClientStateDisposition::Done;
      queue.erase(assertionIter);

      // If there are no more entries queued with this name, remove
      // the name from the states map.
      if (queue.empty()) {
        states_.erase(it);
      } else {
        // Now check to see who is at the front of the queue.  If
        // they are set to asserted and have a payload assigned, they
        // are a state-enter that is pending broadcast of the assertion.
        // We couldn't send it earlier without risking out of order
        // delivery wrt. vacating states.
        auto front = queue.front();
        if (front->disposition == ClientStateDisposition::Asserted &&
            front->enterPayload) {
          front->root->unilateralResponses->enqueue(
              std::move(front->enterPayload));
          front->enterPayload = nullptr;
        }
      }
      return true;
    }
  }

  return false;
}

bool ClientStateAssertions::isFront(
    const std::shared_ptr<ClientStateAssertion>& assertion) const {
  auto it = states_.find(assertion->name);
  if (it == states_.end()) {
    return false;
  }
  auto& queue = it->second;
  if (queue.empty()) {
    return false;
  }
  return queue.front() == assertion;
}

bool ClientStateAssertions::isStateAsserted(w_string stateName) const {
  auto it = states_.find(stateName);
  if (it == states_.end()) {
    return false;
  }
  auto& queue = it->second;
  for (auto& state : queue) {
    if (state->disposition == Asserted) {
      return true;
    }
  }
  return false;
}

} // namespace watchman

static void cmd_state_enter(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed;
  auto client = dynamic_cast<watchman_user_client*>(clientbase);

  auto root = resolveRoot(client, args);

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  if (client->states.find(parsed.name) != client->states.end()) {
    send_error_response(
        client, "state %s is already asserted", parsed.name.c_str());
    return;
  }

  auto assertion = std::make_shared<ClientStateAssertion>(root, parsed.name);

  // Ask the root to track the assertion and maintain ordering.
  // This will throw if the state is already asserted or pending assertion
  // so we do this prior to linking it in to the client.
  root->assertedStates.wlock()->queueAssertion(assertion);

  // Increment state transition counter for this root
  root->stateTransCount++;
  // Record the state assertion in the client
  client->states[parsed.name] = assertion;

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  auto response = make_response();

  response.set({{"root", w_string_to_json(root->root_path)},
                {"state-enter", w_string_to_json(parsed.name)}});
  send_and_dispose_response(client, std::move(response));

  root->cookies
      .sync()
      // Note that it is possible that the sync()
      // might throw.  If that happens the exception will bubble back
      // to the client as an error PDU.
      // after this point, any errors are async and the client is
      // unaware of them.
      .then([assertion, parsed, root](
                watchman::Result<watchman::Unit>&& result) {
        try {
          result.throwIfError();
        } catch (const std::exception& exc) {
          // The sync failed for whatever reason; log it.
          log(ERR, "state-enter sync failed: ", exc.what(), "\n");
          // Don't allow this assertion to clog up and block further
          // attempts.  Mark it as done and remove it from the root.
          // The client side of this will get removed when the client
          // disconnects or attempts to leave the state.
          root->assertedStates.wlock()->removeAssertion(assertion);
          return;
        }
        auto clock = w_string_to_json(root->view()->getCurrentClockString());
        auto payload =
            json_object({{"root", w_string_to_json(root->root_path)},
                         {"clock", std::move(clock)},
                         {"state-enter", w_string_to_json(parsed.name)}});
        if (parsed.metadata) {
          payload.set("metadata", json_ref(parsed.metadata));
        }

        {
          auto wlock = root->assertedStates.wlock();
          assertion->disposition = ClientStateDisposition::Asserted;

          if (wlock->isFront(assertion)) {
            // Broadcast about the state enter
            root->unilateralResponses->enqueue(std::move(payload));
          } else {
            // Defer the broadcast until we are at the front of the queue.
            // removeAssertion() will take care of sending this when this
            // assertion makes it to the front of the queue.
            assertion->enterPayload = payload;
          }
        }
      });
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
  assertion->root->assertedStates.wlock()->removeAssertion(assertion);
  // Increment state transition counter for this root
  assertion->root->stateTransCount++;

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

  auto it = client->states.find(parsed.name);
  if (it == client->states.end()) {
    send_error_response(
        client, "state %s is not asserted", parsed.name.c_str());
    return;
  }

  assertion = it->second.lock();
  if (!assertion) {
    send_error_response(
        client, "state %s was implicitly vacated", parsed.name.c_str());
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

  // Mark as pending leave; we haven't vacated the state until we've
  // seen the sync cookie.
  {
    auto assertedStates = root->assertedStates.wlock();
    if (assertion->disposition == ClientStateDisposition::Done) {
      send_error_response(
          client, "state %s was implicitly vacated", parsed.name.c_str());
      return;
    }
    // Note that there is a potential race here wrt. this state being
    // asserted again by another client and the broadcast
    // of the payload below, because the asserted states lock in
    // scope here cannot be held that long.  We address that race
    // by only broadcasting the enter assertion when it reaches
    // the front of the queue.  That happens in removeAssertion()
    // and also in the post-sync portion of the code in cmd_state_enter().
    assertion->disposition = ClientStateDisposition::PendingLeave;
  }

  // Remove the association from the client.  We'll remove it from the
  // root on the other side of the sync.
  client->states.erase(it);

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  auto response = make_response();
  response.set({{"root", w_string_to_json(root->root_path)},
                {"state-leave", w_string_to_json(parsed.name)}});
  send_and_dispose_response(client, std::move(response));

  root->cookies.sync().then(
      [assertion, parsed, root](watchman::Result<watchman::Unit>&& result) {
        try {
          result.throwIfError();
        } catch (const std::exception& exc) {
          // The sync failed for whatever reason; log it and take no futher
          // action
          log(ERR, "state-leave sync failed: ", exc.what(), "\n");
          return;
        }
        // Notify and exit the state
        leave_state(nullptr, assertion, false, parsed.metadata);
      });
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
