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

#include "watchman/WatchmanConfig.h"
#include "watchman/sockname.h"
#include "watchman/watchman_stream.h"

using namespace watchman;

std::unique_ptr<watchman_stream> w_stm_connect(int timeoutms) {
  // Default to using unix domain sockets unless disabled by config
  auto use_unix_domain = Configuration().getBool("use-unix-domain", true);

  if (use_unix_domain && !disable_unix_socket) {
    auto stm = w_stm_connect_unix(get_unix_sock_name().c_str(), timeoutms);
    if (stm) {
      return stm;
    }
  }

#ifdef _WIN32
  if (!disable_named_pipe) {
    return w_stm_connect_named_pipe(
        get_named_pipe_sock_path().c_str(), timeoutms);
  }
#endif

  return nullptr;
}

int w_poll_events(struct watchman_event_poll* p, int n, int timeoutms) {
#ifdef _WIN32
  if (!p->evt->isSocket()) {
    return w_poll_events_named_pipe(p, n, timeoutms);
  }
#endif
  return w_poll_events_sockets(p, n, timeoutms);
}
