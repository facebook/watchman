/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_H
#define WATCHMAN_H

#include "watchman_system.h"

#include "thirdparty/jansson/jansson.h"
#include "watchman_hash.h"
#include "watchman_ignore.h"
#include "watchman_log.h"
#include "watchman_stream.h"
#include "watchman_string.h"

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_pending_fs;
struct watchman_trigger_command;
struct write_locked_watchman_root;
struct unlocked_watchman_root;
struct read_locked_watchman_root;
typedef struct watchman_root w_root_t;

// Per-watch state for the selected watcher
typedef void *watchman_watcher_t;

#include "watchman_clockspec.h"
#include "watchman_pending.h"
#include "watchman_dir.h"
#include "watchman_watcher.h"
#include "watchman_opendir.h"
#include "watchman_file.h"

#define WATCHMAN_IO_BUF_SIZE 1048576
#define WATCHMAN_BATCH_LIMIT (16*1024)

#include "watchman_root.h"
#include "watchman_pdu.h"
#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_client.h"

#include "watchman_cmd.h"
#include "watchman_config.h"
#include "watchman_trigger.h"

// Returns the name of the filesystem for the specified path
w_string w_fstype(const char *path);

extern char *poisoned_reason;

static inline void w_set_cloexec(int fd)
{
#ifndef _WIN32
  ignore_result(fcntl(fd, F_SETFD, FD_CLOEXEC));
#else
  unused_parameter(fd);
#endif
}

static inline void w_set_nonblock(int fd)
{
#ifndef _WIN32
  ignore_result(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK));
#else
  unused_parameter(fd);
#endif
}

static inline void w_clear_nonblock(int fd)
{
#ifndef _WIN32
  ignore_result(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK));
#else
  unused_parameter(fd);
#endif
}

// Make a temporary file name and open it.
// Marks the file as CLOEXEC
w_stm_t w_mkstemp(char *templ);
char *w_realpath(const char *filename);

#ifndef _WIN32
static inline bool w_path_exists(const char *path) {
  return access(path, F_OK) == 0;
}
#else
bool w_path_exists(const char *path);
#endif

/* We leverage the fact that our aligned pointers will never set the LSB of a
 * pointer value.  We can use the LSB to indicate whether kqueue entries are
 * dirs or files */
#define SET_DIR_BIT(dir)   ((void*)(((intptr_t)dir) | 0x1))
#define IS_DIR_BIT_SET(dir) ((((intptr_t)dir) & 0x1) == 0x1)
#define DECODE_DIR(dir)    ((void*)(((intptr_t)dir) & ~0x1))

void w_mark_dead(pid_t pid);
bool w_reap_children(bool block);
void w_start_reaper(void);
bool w_is_stopping(void);
extern pthread_t reaper_thread;

void w_request_shutdown(void);

void w_cancel_subscriptions_for_root(const w_root_t *root);

static inline uint32_t next_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

#include "watchman_time.h"

extern const char *watchman_tmp_dir;
extern char *watchman_state_file;
extern int dont_save_state;
void w_state_shutdown(void);
void w_state_save(void);
bool w_state_load(void);
bool w_root_save_state(json_ref& state);
bool w_root_load_state(const json_ref& state);
json_ref w_root_trigger_list_to_json(struct read_locked_watchman_root* lock);
json_ref w_root_watch_list_to_json(void);

#ifdef __APPLE__
int w_get_listener_socket_from_launchd(void);
#endif
bool w_listener_prep_inetd(void);
bool w_start_listener(const char *socket_path);
void w_check_my_sock(void);
char** w_argv_copy_from_json(const json_ref& arr, int skip);

#include "watchman_env.h"
#include "watchman_getopt.h"

#ifndef MIN
# define MIN(a, b)  (a) < (b) ? (a) : (b)
#endif
#ifndef MAX
# define MAX(a, b)  (a) > (b) ? (a) : (b)
#endif

#ifdef HAVE_SYS_SIGLIST
# define w_strsignal(val) sys_siglist[(val)]
#else
# define w_strsignal(val) strsignal((val))
#endif

const char *get_sock_name(void);

int w_lstat(const char *path, struct stat *st, bool case_sensitive);

void w_ioprio_set_low(void);
void w_ioprio_set_normal(void);

struct flag_map {
  uint32_t value;
  const char *label;
};
void w_expand_flags(const struct flag_map *fmap, uint32_t flags,
    char *buf, size_t len);

#ifdef __APPLE__
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *ts);
int pthread_rwlock_timedwrlock(
    pthread_rwlock_t* rwlock,
    const struct timespec* ts);
int pthread_rwlock_timedrdlock(
    pthread_rwlock_t* rwlock,
    const struct timespec* ts);
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
