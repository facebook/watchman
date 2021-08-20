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

#pragma once

#include <string>

namespace watchman {

/** Returns the legacy socket name.
 * It is legacy because its meaning is system dependent and
 * a little confusing, but needs to be retained for backwards
 * compatibility reasons as it is exported into the environment
 * in a number of scenarios.
 * You should prefer to use get_unix_sock_name() or
 * get_named_pipe_sock_path() instead
 */
const char* get_sock_name_legacy();

/** Returns the configured unix domain socket path. */
const std::string& get_unix_sock_name();

/** Returns the configured named pipe socket path */
const std::string& get_named_pipe_sock_path();

extern bool disable_unix_socket;
extern bool disable_named_pipe;

} // namespace watchman
