/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdexcept>
#include "watchman/CommandRegistry.h"
#include "watchman/PDU.h"
#include "watchman/watchman_preprocessor.h"
#include "watchman/watchman_system.h"

namespace watchman {
class Client;
class Root;
} // namespace watchman

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
    watchman::Client* client,
    const json_ref& args,
    watchman::CommandFlags mode);
bool try_client_mode_command(const json_ref& cmd, bool pretty);

// Resolve the root. Failure will throw a RootResolveError exception
std::shared_ptr<watchman::Root> resolveRoot(
    watchman::Client* client,
    const json_ref& args);

// Resolve the root, or if not found and the configuration permits,
// attempt to create it. throws RootResolveError on failure.
std::shared_ptr<watchman::Root> resolveOrCreateRoot(
    watchman::Client* client,
    const json_ref& args);

json_ref make_response();
void add_root_warnings_to_response(
    json_ref& response,
    const std::shared_ptr<watchman::Root>& root);

/* vim:ts=2:sw=2:et:
 */
