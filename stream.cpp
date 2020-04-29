/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::unique_ptr<watchman_stream> w_stm_connect(int timeoutms) {
#ifdef _WIN32
  return w_stm_connect_named_pipe(
      get_named_pipe_sock_path().c_str(), timeoutms);
#else
  return w_stm_connect_unix(get_unix_sock_name().c_str(), timeoutms);
#endif
}

int w_poll_events(struct watchman_event_poll* p, int n, int timeoutms) {
#ifdef _WIN32
  if (!p->evt->isSocket()) {
    return w_poll_events_named_pipe(p, n, timeoutms);
  }
#endif
  return w_poll_events_sockets(p, n, timeoutms);
}
