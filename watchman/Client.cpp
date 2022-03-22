/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Client.h"

namespace watchman {

namespace {
// TODO: If used in a hot loop, EdenFS has a faster implementation.
// https://github.com/facebookexperimental/eden/blob/c745d644d969dae1e4c0d184c19320fac7c27ae5/eden/fs/utils/IDGen.h
std::atomic<uint64_t> id_generator{1};
} // namespace

Client::Client() : Client(nullptr) {}

Client::Client(std::unique_ptr<watchman_stream>&& stm)
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

void Client::enqueueResponse(json_ref&& resp, bool ping) {
  responses.emplace_back(std::move(resp));

  if (ping) {
    this->ping->notify();
  }
}

} // namespace watchman
