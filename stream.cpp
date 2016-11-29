/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::unique_ptr<watchman_stream> w_stm_connect(
    const char* path,
    int timeoutms) {
#ifdef _WIN32
  return w_stm_connect_named_pipe(path, timeoutms);
#else
  return w_stm_connect_unix(path, timeoutms);
#endif
}
