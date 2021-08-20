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

#include "watchman/Options.h"

using namespace watchman;

namespace watchman {

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

} // namespace watchman
