/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <folly/Synchronized.h>
#include <string>
#include "watchman/thirdparty/jansson/jansson.h"

extern folly::Synchronized<std::string> poisoned_reason;

bool w_is_stopping();

void w_request_shutdown();

#include "watchman/watchman_time.h"

extern std::string watchman_tmp_dir;
void w_state_shutdown();
void w_state_save();
bool w_state_load();
bool w_root_save_state(json_ref& state);
bool w_root_load_state(const json_ref& state);
json_ref w_root_watch_list_to_json();

#include "watchman/FileDescriptor.h"

#ifdef __APPLE__
watchman::FileDescriptor w_get_listener_socket_from_launchd();
#endif
void w_listener_prep_inetd();
bool w_start_listener();
namespace watchman {
void startSanityCheckThread();
}

#ifdef HAVE_SYS_SIGLIST
#define w_strsignal(val) sys_siglist[(val)]
#else
#define w_strsignal(val) strsignal((val))
#endif

#ifndef _WIN32
/**
 * Gets the group struct for the given group name. The return value may point
 * to a static area so it should be used immediately and not passed to free(3).
 *
 * Returns null on failure.
 */
const struct group* w_get_group(const char* group_name);
#endif // ndef WIN32

/* vim:ts=2:sw=2:et:
 */
