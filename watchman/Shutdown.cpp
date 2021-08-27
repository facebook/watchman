/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/Shutdown.h"
#include <atomic>
#include <vector>
#include "watchman/watchman_stream.h"

namespace {

static std::vector<std::shared_ptr<watchman_event>> listener_thread_events;
static std::atomic<bool> stopping = false;

} // namespace

bool w_is_stopping() {
  return stopping.load(std::memory_order_relaxed);
}

void w_request_shutdown() {
  stopping.store(true, std::memory_order_relaxed);
  // Knock listener thread out of poll/accept
  for (auto& evt : listener_thread_events) {
    evt->notify();
  }
}

void w_push_listener_thread_event(std::shared_ptr<watchman_event> event) {
  listener_thread_events.push_back(std::move(event));
}
