/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::string sock_name;

const char* get_sock_name(void) {
  return sock_name.c_str();
}
