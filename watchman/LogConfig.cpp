// Copyright 2004-present Facebook. All Rights Reserved.

#include "watchman/LogConfig.h"
#include "watchman/Logging.h"

namespace watchman::logging {

int log_level = LogLevel::ERR;
std::string log_name;

} // namespace watchman::logging
