/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Options.h"

using watchman::flags;

bool disable_unix_socket = false;
bool disable_named_pipe = false;

const char* get_sock_name_legacy() {
#ifdef _WIN32
  return flags.named_pipe_path.c_str();
#else
  return flags.unix_sock_name.c_str();
#endif
}

const std::string& get_unix_sock_name() {
  return flags.unix_sock_name;
}

const std::string& get_named_pipe_sock_path() {
  return flags.named_pipe_path;
}
