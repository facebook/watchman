/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_H
#define WATCHMAN_H

#include "watchman_system.h"

#include "watchman_string.h"
#include "thirdparty/jansson/jansson.h"
#include "watchman_hash.h"
#include "watchman_ignore.h"
#include "watchman_stream.h"

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_pending_fs;
struct watchman_trigger_command;
typedef struct watchman_root w_root_t;

#include "watchman_clockspec.h"
#include "watchman_dir.h"
#include "watchman_file.h"
#include "watchman_opendir.h"
#include "watchman_pending.h"
#include "watchman_watcher.h"

#define WATCHMAN_IO_BUF_SIZE 1048576
#define WATCHMAN_BATCH_LIMIT (16 * 1024)

#include "watchman_client.h"
#include "watchman_pdu.h"
#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_root.h"

#include "watchman_cmd.h"
#include "watchman_config.h"
#include "watchman_trigger.h"

// Returns the name of the filesystem for the specified path
w_string w_fstype(const char* path);

extern folly::Synchronized<std::string> poisoned_reason;

#ifndef _WIN32
static inline bool w_path_exists(const char* path) {
  return access(path, F_OK) == 0;
}
#else
bool w_path_exists(const char* path);
#endif

/* We leverage the fact that our aligned pointers will never set the LSB of a
 * pointer value.  We can use the LSB to indicate whether kqueue entries are
 * dirs or files */
#define SET_DIR_BIT(dir) ((void*)(((intptr_t)dir) | 0x1))
#define IS_DIR_BIT_SET(dir) ((((intptr_t)dir) & 0x1) == 0x1)
#define DECODE_DIR(dir) ((void*)(((intptr_t)dir) & ~0x1))

bool w_is_stopping(void);

void w_request_shutdown(void);

#include "watchman_time.h"

extern const char* watchman_tmp_dir;
extern std::string watchman_state_file;
extern int dont_save_state;
void w_state_shutdown(void);
void w_state_save(void);
bool w_state_load(void);
bool w_root_save_state(json_ref& state);
bool w_root_load_state(const json_ref& state);
json_ref w_root_watch_list_to_json(void);

#include "FileDescriptor.h"

#ifdef __APPLE__
watchman::FileDescriptor w_get_listener_socket_from_launchd();
#endif
void w_listener_prep_inetd();
bool w_start_listener();
namespace watchman {
void startSanityCheckThread(void);
}

#include "watchman_getopt.h"

#ifdef HAVE_SYS_SIGLIST
#define w_strsignal(val) sys_siglist[(val)]
#else
#define w_strsignal(val) strsignal((val))
#endif

extern std::string unix_sock_name;
extern std::string named_pipe_path;

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

struct flag_map {
  uint32_t value;
  const char* label;
};
void w_expand_flags(
    const struct flag_map* fmap,
    uint32_t flags,
    char* buf,
    size_t len);

#endif

/**
 * Provide a way to query (and eventually modify) command line arguments
 *
 * This is not thread-safe and should only be invoked from main()
 */
struct watchman_command_handler_def* lookup(const w_string& cmd_name, int mode);

/* vim:ts=2:sw=2:et:
 */
