/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman/watcher/Watcher.h"

namespace watchman {

Watcher::Watcher(const char* name, unsigned flags) : name(name), flags(flags) {}

Watcher::~Watcher() {}

bool Watcher::startWatchFile(watchman_file*) {
  return true;
}

bool Watcher::start(const std::shared_ptr<watchman_root>&) {
  return true;
}

} // namespace watchman
