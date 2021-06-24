/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/WatchmanConfig.h"
#include "watchman/watchman.h"
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
