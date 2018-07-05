/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

char* sock_name = NULL;

const char* get_sock_name(void) {
  return sock_name;
}
