/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
