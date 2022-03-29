/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"
#include "watchman/MapUtil.h"
#include "watchman/QueryableView.h"
#include "watchman/ThreadPool.h"
#include "watchman/query/parse.h"
#include "watchman/root/Root.h"
#include "watchman/watchman_cmd.h"
#include "watchman/watchman_system.h"

using namespace watchman;
using ms = std::chrono::milliseconds;

struct state_arg {
  w_string name;
  ms sync_timeout;
  json_ref metadata;
};

// Parses the args for state-enter and state-leave
static bool parse_state_arg(
    Client* client,
    const json_ref& args,
    struct state_arg* parsed) {
  parsed->sync_timeout = kDefaultQuerySyncTimeout;
  parsed->metadata = nullptr;
  parsed->name = nullptr;

  if (json_array_size(args) != 3) {
    client->sendErrorResponse(
        "invalid number of arguments, expected 3, got {}",
        json_array_size(args));
    return false;
  }

  const auto& state_args = args.at(2);

  // [cmd, root, statename]
  if (state_args.isString()) {
    parsed->name = json_to_w_string(state_args);
    return true;
  }

  // [cmd, root, {name:, metadata:, sync_timeout:}]
  parsed->name = json_to_w_string(state_args.get("name"));
  parsed->metadata = state_args.get_default("metadata");
  parsed->sync_timeout =
      ms(state_args
             .get_default(
                 "sync_timeout", json_integer(parsed->sync_timeout.count()))
             .asInt());

  if (parsed->sync_timeout < ms::zero()) {
    client->sendErrorResponse("sync_timeout must be >= 0");
    return false;
  }

  return true;
}

namespace watchman {

static void cmd_state_enter(Client* clientbase, const json_ref& args) {
  struct state_arg parsed;
  auto client = dynamic_cast<UserClient*>(clientbase);

  auto root = resolveRoot(client, args);

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  if (client->states.find(parsed.name) != client->states.end()) {
    client->sendErrorResponse("state {} is already asserted", parsed.name);
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

  response.set(
      {{"root", w_string_to_json(root->root_path)},
       {"state-enter", w_string_to_json(parsed.name)}});
  client->enqueueResponse(std::move(response));

  root->cookies
      .sync()
      // Note that it is possible that the sync()
      // might throw.  If that happens the exception will bubble back
      // to the client as an error PDU.
      // after this point, any errors are async and the client is
      // unaware of them.
      .thenTry([assertion, parsed, root](
                   folly::Try<CookieSync::SyncResult>&& result) {
        try {
          result.throwUnlessValue();
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
        auto payload = json_object(
            {{"root", w_string_to_json(root->root_path)},
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

} // namespace watchman

W_CMD_REG("state-enter", cmd_state_enter, CMD_DAEMON, w_cmd_realpath_root)

static void cmd_state_leave(Client* clientbase, const json_ref& args) {
  struct state_arg parsed;
  // This is a weak reference to the assertion.  This is safe because only this
  // client can delete this assertion, and this function is only executed by
  // the thread that owns this client.
  std::shared_ptr<ClientStateAssertion> assertion;
  auto client = dynamic_cast<UserClient*>(clientbase);

  auto root = resolveRoot(client, args);

  if (!parse_state_arg(client, args, &parsed)) {
    return;
  }

  auto it = client->states.find(parsed.name);
  if (it == client->states.end()) {
    client->sendErrorResponse("state {} is not asserted", parsed.name);
    return;
  }

  assertion = it->second.lock();
  if (!assertion) {
    client->sendErrorResponse("state {} was implicitly vacated", parsed.name);
    return;
  }

  // Sanity check ownership
  if (mapGetDefault(client->states, parsed.name).lock() != assertion) {
    client->sendErrorResponse(
        "state {} was not asserted by this session", parsed.name);
    return;
  }

  // Mark as pending leave; we haven't vacated the state until we've
  // seen the sync cookie.
  {
    auto assertedStates = root->assertedStates.wlock();
    if (assertion->disposition == ClientStateDisposition::Done) {
      client->sendErrorResponse("state {} was implicitly vacated", parsed.name);
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
  response.set(
      {{"root", w_string_to_json(root->root_path)},
       {"state-leave", w_string_to_json(parsed.name)}});
  client->enqueueResponse(std::move(response));

  root->cookies.sync().thenTry(
      [assertion, parsed, root](folly::Try<CookieSync::SyncResult>&& result) {
        try {
          result.throwUnlessValue();
        } catch (const std::exception& exc) {
          // The sync failed for whatever reason; log it and take no futher
          // action
          log(ERR, "state-leave sync failed: ", exc.what(), "\n");
          return;
        }
        // Notify and exit the state
        w_leave_state(nullptr, assertion, false, parsed.metadata);
      });
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
