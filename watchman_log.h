/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// You are encouraged to include "Logging.h" and use those functions rather than
// use these.  The functions in this file are best suited to low level or early
// bootstrapping situations.

#ifndef WATCHMAN_LOG_H
#define WATCHMAN_LOG_H

#include <string>

extern int log_level;
extern std::string log_name;
const char* w_set_thread_name_impl(std::string&& name);

void w_setup_signal_handlers(void);

#endif
