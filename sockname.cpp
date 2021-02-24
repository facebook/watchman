/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::string unix_sock_name;
std::string named_pipe_path;

bool disable_unix_socket = false;
bool disable_named_pipe = false;

const char* get_sock_name_legacy() {
#ifdef _WIN32
  return named_pipe_path.c_str();
#else
  return unix_sock_name.c_str();
#endif
}

const std::string& get_unix_sock_name() {
  return unix_sock_name;
}

const std::string& get_named_pipe_sock_path() {
  return named_pipe_path;
}
