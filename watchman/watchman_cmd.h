/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <stdexcept>
#include "watchman/CommandRegistry.h"

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
    enum w_pdu_type output_pdu,
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
