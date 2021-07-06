/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_H
#define WATCHMAN_H

#include "watchman/watchman_system.h"

#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman_string.h"
#include "watchman_hash.h"
#include "watchman_stream.h"

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_trigger_command;

#include "watchman/watchman_dir.h"
#include "watchman/watchman_file.h"

#define WATCHMAN_IO_BUF_SIZE 1048576
#define WATCHMAN_BATCH_LIMIT (16 * 1024)

#include "watchman/watchman_perf.h"
#include "watchman_client.h"
#include "watchman_pdu.h"
#include "watchman_query.h"
#include "watchman_root.h"

#include "watchman/watchman_cmd.h"
#include "watchman_trigger.h"

extern folly::Synchronized<std::string> poisoned_reason;

bool w_is_stopping();

void w_request_shutdown();

#include "watchman/watchman_time.h"

extern std::string watchman_tmp_dir;
extern std::string watchman_state_file;
extern int dont_save_state;
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

#include "watchman_getopt.h"

#ifdef HAVE_SYS_SIGLIST
#define w_strsignal(val) sys_siglist[(val)]
#else
#define w_strsignal(val) strsignal((val))
#endif

extern std::string unix_sock_name;
extern std::string named_pipe_path;
extern bool disable_unix_socket;
extern bool disable_named_pipe;

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

#ifndef _WIN32
/**
 * Gets the group struct for the given group name. The return value may point
 * to a static area so it should be used immediately and not passed to free(3).
 *
 * Returns null on failure.
 */
const struct group* w_get_group(const char* group_name);
#endif // ndef WIN32

#endif

/* vim:ts=2:sw=2:et:
 */
