/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/Connect.h"
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
