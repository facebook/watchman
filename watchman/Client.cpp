/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"
#include "watchman/Logging.h"
#include "watchman/MapUtil.h"
#include "watchman/QueryableView.h"
#include "watchman/root/Root.h"

namespace watchman {

namespace {

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

void Client::enqueueResponse(json_ref resp, bool ping) {
  responses.emplace_back(std::move(resp));

  if (ping) {
    this->ping->notify();
  }
}

UserClient::UserClient(std::unique_ptr<watchman_stream> stm)
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
