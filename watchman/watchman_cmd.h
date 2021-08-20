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

#include <stdexcept>
#include "watchman/CommandRegistry.h"
#include "watchman/watchman_pdu.h"
#include "watchman/watchman_preprocessor.h"
#include "watchman/watchman_system.h"

struct watchman_root;

// For commands that take the root dir as the second parameter,
// realpath's that parameter on the client side and updates the
// argument list
void w_cmd_realpath_root(json_ref& args);

// Try to find a project root that contains the path `resolved`. If found,
// modify `resolved` to hold the path to the root project and return true.
// Else, return false.
// root_files should be derived from a call to cfg_compute_root_files, and it
// should not be null.  cfg_compute_root_files ensures that .watchmanconfig is
// first in the returned list of files.  This is important because it is the
// definitive indicator for the location of the project root.
bool find_project_root(
    const json_ref& root_files,
    w_string_piece& resolved,
    w_string_piece& relpath);

void preprocess_command(
    json_ref& args,
    w_pdu_type output_pdu,
    uint32_t output_capabilities);
bool dispatch_command(
    struct watchman_client* client,
    const json_ref& args,
    int mode);
bool try_client_mode_command(const json_ref& cmd, bool pretty);

void send_error_response(
    struct watchman_client* client,
    WATCHMAN_FMT_STRING(const char* fmt),
    ...) WATCHMAN_FMT_ATTR(2, 3);
void send_and_dispose_response(
    struct watchman_client* client,
    json_ref&& response);
bool enqueue_response(
    struct watchman_client* client,
    json_ref&& json,
    bool ping);

// Resolve the root. Failure will throw a RootResolveError exception
std::shared_ptr<watchman_root> resolveRoot(
    struct watchman_client* client,
    const json_ref& args);

// Resolve the root, or if not found and the configuration permits,
// attempt to create it. throws RootResolveError on failure.
std::shared_ptr<watchman_root> resolveOrCreateRoot(
    struct watchman_client* client,
    const json_ref& args);

json_ref make_response();
void add_root_warnings_to_response(
    json_ref& response,
    const std::shared_ptr<watchman_root>& root);

bool clock_id_string(
    uint32_t root_number,
    uint32_t ticks,
    char* buf,
    size_t bufsize);

/* vim:ts=2:sw=2:et:
 */
