/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"

#include <folly/MapUtil.h>

#include "watchman/Logging.h"
#include "watchman/MapUtil.h"
#include "watchman/QueryableView.h"
#include "watchman/Shutdown.h"
#include "watchman/root/Root.h"
#include "watchman/watchman_cmd.h"

namespace watchman {

namespace {

constexpr size_t kResponseLogLimit = 0;

folly::Synchronized<std::unordered_set<UserClient*>> clients;

// TODO: If used in a hot loop, EdenFS has a faster implementation.
// https://github.com/facebookexperimental/eden/blob/c745d644d969dae1e4c0d184c19320fac7c27ae5/eden/fs/utils/IDGen.h
std::atomic<uint64_t> id_generator{1};
} // namespace

Client::Client() : Client(nullptr) {}

Client::Client(std::unique_ptr<watchman_stream> stm)
    : unique_id{id_generator++},
      stm(std::move(stm)),
      ping(
#ifdef _WIN32
          (this->stm &&
           this->stm->getFileDescriptor().fdType() ==
               FileDescriptor::FDType::Socket)
              ? w_event_make_sockets()
              : w_event_make_named_pipe()
#else
          w_event_make_sockets()
#endif
      ) {
  logf(DBG, "accepted client:stm={}\n", fmt::ptr(this->stm.get()));
}

Client::~Client() {
  debugSub.reset();
  errorSub.reset();

  logf(DBG, "client_delete {}\n", unique_id);

  if (stm) {
    stm->shutdown();
  }
}

void Client::enqueueResponse(json_ref resp) {
  responses.emplace_back(std::move(resp));
}

void Client::sendErrorResponse(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  auto errorText = w_string::vprintf(fmt, ap);
  va_end(ap);

  auto resp = make_response();
  resp.set("error", w_string_to_json(errorText));

  if (perf_sample) {
    perf_sample->add_meta("error", w_string_to_json(errorText));
  }

  if (current_command) {
    auto command = json_dumps(current_command, 0);
    watchman::log(
        watchman::ERR,
        "send_error_response: ",
        command,
        ", failed: ",
        errorText,
        "\n");
  } else {
    watchman::log(watchman::ERR, "send_error_response: ", errorText, "\n");
  }

  enqueueResponse(std::move(resp));
}

void UserClient::create(std::unique_ptr<watchman_stream> stm) {
  auto uc = std::make_shared<UserClient>(PrivateBadge{}, std::move(stm));

  // Start a thread for the client.
  //
  // We used to use libevent for this, but we have a low volume of concurrent
  // clients and the json parse/encode APIs are not easily used in a
  // non-blocking server architecture.
  //
  // The thread holds a reference count for its life, so the shared_ptr must be
  // created before the thread is started.
  std::thread{[uc] { clientThread(uc); }}.detach();
}

UserClient::UserClient(PrivateBadge, std::unique_ptr<watchman_stream> stm)
    : Client(std::move(stm)) {
  clients.wlock()->insert(this);
}

UserClient::~UserClient() {
  clients.wlock()->erase(this);

  /* cancel subscriptions */
  subscriptions.clear();

  w_client_vacate_states(this);
}

std::vector<std::shared_ptr<UserClient>> UserClient::getAllClients() {
  std::vector<std::shared_ptr<UserClient>> v;

  auto lock = clients.rlock();
  v.reserve(lock->size());
  for (auto& c : *lock) {
    v.push_back(std::static_pointer_cast<UserClient>(c->shared_from_this()));
  }
  return v;
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
void UserClient::clientThread(std::shared_ptr<UserClient> client) noexcept {
  // Keep a persistent vector around so that we can avoid allocating
  // and releasing heap memory when we collect items from the publisher
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> pending;

  client->stm->setNonBlock(true);
  w_set_thread_name(
      "client=",
      client->unique_id,
      ":stm=",
      uintptr_t(client->stm.get()),
      ":pid=",
      client->stm->getPeerProcessID());

  client->client_is_owner = client->stm->peerIsOwner();

  struct watchman_event_poll pfd[2];
  pfd[0].evt = client->stm->getEvents();
  pfd[1].evt = client->ping.get();

  bool client_alive = true;
  while (!w_is_stopping() && client_alive) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    ignore_result(w_poll_events(pfd, 2, 2000));
    if (w_is_stopping()) {
      break;
    }

    if (pfd[0].ready) {
      json_error_t jerr;
      auto request = client->reader.decodeNext(client->stm.get(), &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        if (client->reader.wpos == client->reader.rpos) {
          // If they disconnected in between PDUs, no need to log
          // any error
          goto disconnected;
        }
        client->sendErrorResponse(
            "invalid json at position %d: %s", jerr.position, jerr.text);
        logf(ERR, "invalid data from client: {}\n", jerr.text);

        goto disconnected;
      } else if (request) {
        client->pdu_type = client->reader.pdu_type;
        client->capabilities = client->reader.capabilities;
        dispatch_command(client.get(), request, CMD_DAEMON);
      }
    }

    if (pfd[1].ready) {
      while (client->ping->testAndClear()) {
        // Enqueue refs to pending log payloads
        pending.clear();
        getPending(pending, client->debugSub, client->errorSub);
        for (auto& item : pending) {
          client->enqueueResponse(json_ref(item->payload));
        }

        // Maybe we have subscriptions to dispatch?
        std::vector<w_string> subsToDelete;
        for (auto& subiter : client->unilateralSub) {
          auto sub = subiter.first;
          auto subStream = subiter.second;

          watchman::log(
              watchman::DBG, "consider fan out sub ", sub->name, "\n");

          pending.clear();
          subStream->getPending(pending);
          bool seenSettle = false;
          for (auto& item : pending) {
            auto dumped = json_dumps(item->payload, 0);
            watchman::log(
                watchman::DBG,
                "Unilateral payload for sub ",
                sub->name,
                " ",
                dumped,
                "\n");

            if (item->payload.get_default("canceled")) {
              watchman::log(
                  watchman::ERR,
                  "Cancel subscription ",
                  sub->name,
                  " due to root cancellation\n");

              auto resp = make_response();
              resp.set(
                  {{"root", item->payload.get_default("root")},
                   {"unilateral", json_true()},
                   {"canceled", json_true()},
                   {"subscription", w_string_to_json(sub->name)}});
              client->enqueueResponse(std::move(resp));
              // Remember to cancel this subscription.
              // We can't do it in this loop because that would
              // invalidate the iterators and cause a headache.
              subsToDelete.push_back(sub->name);
              continue;
            }

            if (item->payload.get_default("state-enter") ||
                item->payload.get_default("state-leave")) {
              auto resp = make_response();
              json_object_update(item->payload, resp);
              // We have the opportunity to populate additional response
              // fields here (since we don't want to block the command).
              // We don't populate the fat clock for SCM aware queries
              // because determination of mergeBase could add latency.
              resp.set(
                  {{"unilateral", json_true()},
                   {"subscription", w_string_to_json(sub->name)}});
              client->enqueueResponse(std::move(resp));

              watchman::log(
                  watchman::DBG,
                  "Fan out subscription state change for ",
                  sub->name,
                  "\n");
              continue;
            }

            if (!sub->debug_paused && item->payload.get_default("settled")) {
              seenSettle = true;
              continue;
            }
          }

          if (seenSettle) {
            sub->processSubscription();
          }
        }

        for (auto& name : subsToDelete) {
          client->unsubByName(name);
        }
      }
    }

    /* now send our response(s) */
    while (!client->responses.empty() && client_alive) {
      auto& response_to_send = client->responses.front();

      client->stm->setNonBlock(false);
      /* Return the data in the same format that was used to ask for it.
       * Update client liveness based on send success.
       */
      client_alive = client->writer.pduEncodeToStream(
          client->pdu_type,
          client->capabilities,
          response_to_send,
          client->stm.get());
      client->stm->setNonBlock(true);

      json_ref subscriptionValue = response_to_send.get_default("subscription");
      if (kResponseLogLimit && subscriptionValue &&
          subscriptionValue.isString() &&
          json_string_value(subscriptionValue)) {
        auto subscriptionName = json_to_w_string(subscriptionValue);
        if (auto* sub =
                folly::get_ptr(client->subscriptions, subscriptionName)) {
          if ((*sub)->lastResponses.size() >= kResponseLogLimit) {
            (*sub)->lastResponses.pop_front();
          }
          (*sub)->lastResponses.push_back(ClientSubscription::LoggedResponse{
              std::chrono::system_clock::now(), response_to_send});
        }
      }

      client->responses.pop_front();
    }
  }

disconnected:
  w_set_thread_name(
      "NOT_CONN:client=",
      client->unique_id,
      ":stm=",
      uintptr_t(client->stm.get()),
      ":pid=",
      client->stm->getPeerProcessID());

  // TODO: Mark client state as THREAD_SHUTTING_DOWN
}

} // namespace watchman

void w_leave_state(
    watchman::UserClient* client,
    std::shared_ptr<watchman::ClientStateAssertion> assertion,
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
void w_client_vacate_states(watchman::UserClient* client) {
  while (!client->states.empty()) {
    auto it = client->states.begin();
    auto assertion = it->second.lock();

    if (!assertion) {
      client->states.erase(it->first);
      continue;
    }

    auto root = assertion->root;

    logf(
        watchman::ERR,
        "implicitly vacating state {} on {} due to client disconnect\n",
        assertion->name,
        root->root_path);

    // This will delete the state from client->states and invalidate
    // the iterator.
    w_leave_state(client, assertion, true, nullptr);
  }
}
