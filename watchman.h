/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_H
#define WATCHMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define _GNU_SOURCE 1
#include "config.h"

#include <assert.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <stdint.h>
#include <sys/stat.h>
#if HAVE_SYS_INOTIFY_H
# include <sys/inotify.h>
#endif
#if HAVE_SYS_EVENT_H
# include <sys/event.h>
#endif
#if HAVE_PORT_H
# include <port.h>
#endif
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#ifndef _WIN32
#include <grp.h>
#include <libgen.h>
#endif
#include <inttypes.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <fcntl.h>
#if defined(__linux__) && !defined(O_CLOEXEC)
# define O_CLOEXEC   02000000 /* set close_on_exec, from asm/fcntl.h */
#endif
#ifndef O_CLOEXEC
# define O_CLOEXEC 0
#endif
#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#endif
#ifdef HAVE_PCRE_H
# include <pcre.h>
#endif
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif
#ifndef _WIN32
#include <sys/uio.h>
#include <pwd.h>
#include <sysexits.h>
#endif
#include <spawn.h>
#include <stddef.h>
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifndef _WIN32
// Not explicitly exported on Darwin, so we get to define it.
extern char **environ;
#endif

#ifdef _WIN32
# define PRIsize_t "Iu"
#else
# define PRIsize_t "zu"
#endif

#ifndef WATCHMAN_DIR_SEP
# define WATCHMAN_DIR_SEP '/'
# define WATCHMAN_DIR_DOT '.'
#endif

#ifdef _WIN32
# define PRIsize_t "Iu"
#else
# define PRIsize_t "zu"
#endif

#if defined(__clang__)
# if __has_feature(address_sanitizer)
#  define WATCHMAN_ASAN 1
# endif
#elif defined (__GNUC__) && \
      (((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ >= 5)) && \
      __SANITIZE_ADDRESS__
# define WATCHMAN_ASAN 1
#endif

#ifndef WATCHMAN_ASAN
# define WATCHMAN_ASAN 0
#endif

extern char *poisoned_reason;

#include "watchman_string.h"
#include "watchman_hash.h"
#include "watchman_stream.h"
#include "watchman_ignore.h"
#include "watchman_log.h"


#ifdef HAVE_CORESERVICES_CORESERVICES_H
# include <CoreServices/CoreServices.h>
# if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
#  define HAVE_FSEVENTS 0
# else
#  define HAVE_FSEVENTS 1
# endif
#endif

// We make use of constructors to glue together modules
// without maintaining static lists of things in the build
// configuration.  These are helpers to make this work
// more portably
#ifdef _WIN32
#pragma section(".CRT$XCU", read)
# define w_ctor_fn_type(sym) void __cdecl sym(void)
# define w_ctor_fn_reg(sym) \
  static __declspec(allocate(".CRT$XCU")) \
    void (__cdecl * w_paste1(sym, _reg))(void) = sym;
#else
# define w_ctor_fn_type(sym) \
  __attribute__((constructor)) void sym(void)
# define w_ctor_fn_reg(sym) /* not needed */
#endif

/* sane, reasonably large filename size that we'll use
 * throughout; POSIX seems to define smallish buffers
 * that seem risky */
#define WATCHMAN_NAME_MAX   4096

// rpmbuild may enable fortify which turns on
// warn_unused_result on a number of system functions.
// This gives us a reasonably clean way to suppress
// these warnings when we're using stack protection.
#if __USE_FORTIFY_LEVEL > 0
# define ignore_result(x) \
  do { __typeof__(x) _res = x; (void)_res; } while(0)
#elif _MSC_VER >= 1400
# define ignore_result(x) \
  do { int _res = (int)x; (void)_res; } while(0)
#else
# define ignore_result(x) x
#endif

// self-documenting hint to the compiler that we didn't use it
#define unused_parameter(x)  (void)x

static inline void w_refcnt_add(volatile long *refcnt)
{
  (void)__sync_fetch_and_add(refcnt, 1);
}

/* returns true if we deleted the last ref */
static inline bool w_refcnt_del(volatile long *refcnt)
{
  return __sync_add_and_fetch(refcnt, -1) == 0;
}

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
bool w_is_path_absolute(const char *path);

#ifndef _WIN32
static inline bool w_path_exists(const char *path) {
  return access(path, F_OK) == 0;
}
#else
bool w_path_exists(const char *path);
#endif

#define HINT_NUM_DIRS 128*1024
#define CFG_HINT_NUM_DIRS "hint_num_dirs"

/* We leverage the fact that our aligned pointers will never set the LSB of a
 * pointer value.  We can use the LSB to indicate whether kqueue entries are
 * dirs or files */
#define SET_DIR_BIT(dir)   ((void*)(((intptr_t)dir) | 0x1))
#define IS_DIR_BIT_SET(dir) ((((intptr_t)dir) & 0x1) == 0x1)
#define DECODE_DIR(dir)    ((void*)(((intptr_t)dir) & ~0x1))

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_pending_fs;
struct watchman_trigger_command;
typedef struct watchman_root w_root_t;

// Per-watch state for the selected watcher
typedef void *watchman_watcher_t;

struct watchman_clock {
  uint32_t ticks;
  struct timeval tv;
};
typedef struct watchman_clock w_clock_t;

#define W_PENDING_RECURSIVE   1
#define W_PENDING_VIA_NOTIFY 2
#define W_PENDING_CRAWL_ONLY  4
struct watchman_pending_fs {
  struct watchman_pending_fs *next, *prev;
  w_string_t *path;
  struct timeval now;
  int flags;
};

struct watchman_pending_collection {
  struct watchman_pending_fs *pending;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  bool pinged;
  art_tree tree;
};

bool w_pending_coll_init(struct watchman_pending_collection *coll);
void w_pending_coll_destroy(struct watchman_pending_collection *coll);
void w_pending_coll_drain(struct watchman_pending_collection *coll);
void w_pending_coll_lock(struct watchman_pending_collection *coll);
void w_pending_coll_unlock(struct watchman_pending_collection *coll);
bool w_pending_coll_add(struct watchman_pending_collection *coll,
    w_string_t *path, struct timeval now, int flags);
bool w_pending_coll_add_rel(struct watchman_pending_collection *coll,
    struct watchman_dir *dir, const char *name,
    struct timeval now, int flags);
void w_pending_coll_append(struct watchman_pending_collection *target,
    struct watchman_pending_collection *src);
struct watchman_pending_fs *w_pending_coll_pop(
    struct watchman_pending_collection *coll);
bool w_pending_coll_lock_and_wait(struct watchman_pending_collection *coll,
    int timeoutms);
void w_pending_coll_ping(struct watchman_pending_collection *coll);
uint32_t w_pending_coll_size(struct watchman_pending_collection *coll);
void w_pending_fs_free(struct watchman_pending_fs *p);

struct watchman_dir {
  /* full path */
  w_string_t *path;
  /* files contained in this dir (keyed by file->name) */
  w_ht_t *files;
  /* child dirs contained in this dir (keyed by dir->path) */
  w_ht_t *dirs;
  // If we think this dir was deleted, we'll avoid recursing
  // to its children when processing deletes
  bool last_check_existed;
};

struct watchman_ops {
  // What's it called??
  const char *name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if renames do not reliably report the individual
  // files renamed in the hierarchy
#define WATCHER_COALESCED_RENAME 2
  unsigned flags;

  // Perform watcher-specific initialization for a watched root.
  // Do not start threads here
  bool (*root_init)(w_root_t *root, char **errmsg);

  // Start up threads or similar.  Called in the context of the
  // notify thread
  bool (*root_start)(w_root_t *root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  void (*root_dtor)(w_root_t *root);

  // Initiate an OS-level watch on the provided file
  bool (*root_start_watch_file)(w_root_t *root, struct watchman_file *file);

  // Cancel an OS-level watch on the provided file
  void (*root_stop_watch_file)(w_root_t *root, struct watchman_file *file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or NULL on error
  struct watchman_dir_handle *(*root_start_watch_dir)(
      w_root_t *root, struct watchman_dir *dir, struct timeval now,
      const char *path);

  // Cancel an OS-level watch on the provided dir
  void (*root_stop_watch_dir)(w_root_t *root, struct watchman_dir *dir);

  // Signal any threads to terminate.  Do not join them here.
  void (*root_signal_threads)(w_root_t *root);

  // Consume any available notifications.  If there are none pending,
  // does not block.
  bool (*root_consume_notify)(w_root_t *root,
      struct watchman_pending_collection *coll);

  // Wait for an inotify event to become available
  bool (*root_wait_notify)(w_root_t *root, int timeoutms);

  // Called when freeing a file node
  void (*file_free)(struct watchman_file *file);
};

bool w_watcher_init(w_root_t *root, char **errmsg);

struct watchman_stat {
  struct timespec atime, mtime, ctime;
  off_t size;
  mode_t mode;
  uid_t uid;
  gid_t gid;
  ino_t ino;
  dev_t dev;
  nlink_t nlink;
};

/* opaque (system dependent) type for walking dir contents */
struct watchman_dir_handle;

struct watchman_dir_ent {
  bool has_stat;
  char *d_name;
  struct watchman_stat stat;
};

struct watchman_dir_handle *w_dir_open(const char *path);
struct watchman_dir_ent *w_dir_read(struct watchman_dir_handle *dir);
void w_dir_close(struct watchman_dir_handle *dir);
int w_dir_fd(struct watchman_dir_handle *dir);

struct watchman_file {
  /* our name within the parent dir */
  w_string_t *name;
  /* the parent dir */
  struct watchman_dir *parent;

  /* linkage to files ordered by changed time */
  struct watchman_file *prev, *next;

  /* linkage to files ordered by common suffix */
  struct watchman_file *suffix_prev, *suffix_next;

  /* the time we last observed a change to this file */
  w_clock_t otime;
  /* the time we first observed this file OR the time
   * that this file switched from !exists to exists.
   * This is thus the "created time" */
  w_clock_t ctime;

  /* whether we believe that this file still exists */
  bool exists;
  /* whether we think this file might not exist */
  bool maybe_deleted;

  /* cache stat results so we can tell if an entry
   * changed */
  struct watchman_stat stat;

  /* the symbolic link target of this file.
   * Can be NULL if not a symlink, or we failed to read the target */
  w_string_t *symlink_target;
};

#define WATCHMAN_COOKIE_PREFIX ".watchman-cookie-"
struct watchman_query_cookie {
  pthread_cond_t cond;
  bool seen;
};

#define WATCHMAN_IO_BUF_SIZE 1048576
#define WATCHMAN_BATCH_LIMIT (16*1024)
#define DEFAULT_SETTLE_PERIOD 20
#define DEFAULT_QUERY_SYNC_MS 60000

/* Prune out nodes that were deleted roughly 12-36 hours ago */
#define DEFAULT_GC_AGE (86400/2)
#define DEFAULT_GC_INTERVAL 86400

/* Idle out watches that haven't had activity in several days */
#define DEFAULT_REAP_AGE (86400*5)

struct watchman_root {
  long refcnt;

  /* path to root */
  w_string_t *root_path;
  bool case_sensitive;

  /* our locking granularity is per-root */
  pthread_mutex_t lock;
  const char *lock_reason;
  pthread_t notify_thread;
  pthread_t io_thread;

  /* map of rule id => struct watchman_trigger_command */
  w_ht_t *commands;

  /* path to the query cookie dir */
  w_string_t *query_cookie_dir;
  w_string_t *query_cookie_prefix;
  w_ht_t *query_cookies;

  struct watchman_ignore ignore;

  int trigger_settle;
  int gc_interval;
  int gc_age;
  int idle_reap_age;

  /* config options loaded via json file */
  json_t *config_file;

  /* how many times we've had to recrawl */
  int recrawl_count;
  w_string_t *last_recrawl_reason;

  // Why we failed to watch
  w_string_t *failure_reason;

  // Last ad-hoc warning message
  w_string_t *warning;

  /* queue of items that we need to stat/process */
  struct watchman_pending_collection pending;

  // map of state name => watchman_client_state_assertion for
  // asserted states
  w_ht_t *asserted_states;

  /* the watcher that we're using for this root */
  struct watchman_ops *watcher_ops;

  /* --- everything below this point will be reset on w_root_init --- */
  bool _init_sentinel_;

  /* root number */
  uint32_t number;

  // Watcher specific state
  watchman_watcher_t watch;

  /* map of dir name to a dir */
  w_ht_t *dirname_to_dir;

  /* the most recently changed file */
  struct watchman_file *latest_file;

  /* current tick */
  uint32_t ticks;

  bool done_initial;
  /* if true, we've decided that we should re-crawl the root
   * for the sake of ensuring consistency */
  bool should_recrawl;
  bool cancelled;

  /* map of cursor name => last observed tick value */
  w_ht_t *cursors;

  /* map of filename suffix => watchman_file at the head
   * of the suffix index.  Linkage via suffix_next */
  w_ht_t *suffixes;

  uint32_t next_cmd_id;
  uint32_t last_trigger_tick;
  uint32_t pending_trigger_tick;
  uint32_t pending_sub_tick;
  uint32_t last_age_out_tick;
  time_t last_age_out_timestamp;
  time_t last_cmd_timestamp;
  time_t last_reap_timestamp;
};

enum w_pdu_type {
  need_data,
  is_json_compact,
  is_json_pretty,
  is_bser,
  is_bser_v2
};

struct watchman_json_buffer {
  char *buf;
  uint32_t allocd;
  uint32_t rpos, wpos;
  enum w_pdu_type pdu_type;
};

typedef struct bser_ctx {
  uint32_t bser_version;
  uint32_t bser_capabilities;
  json_dump_callback_t dump;
} bser_ctx_t;

typedef struct watchman_json_buffer w_jbuffer_t;
bool w_json_buffer_init(w_jbuffer_t *jr);
void w_json_buffer_reset(w_jbuffer_t *jr);
void w_json_buffer_free(w_jbuffer_t *jr);
json_t *w_json_buffer_next(w_jbuffer_t *jr, w_stm_t stm, json_error_t *jerr);
bool w_json_buffer_passthru(w_jbuffer_t *jr,
    enum w_pdu_type output_pdu,
    w_jbuffer_t *output_pdu_buf,
    w_stm_t stm);
bool w_json_buffer_write(w_jbuffer_t *jr, w_stm_t stm, json_t *json, int flags);
bool w_json_buffer_write_bser(uint32_t bser_version, uint32_t bser_capabilities,
    w_jbuffer_t *jr, w_stm_t stm, json_t *json);
bool w_ser_write_pdu(enum w_pdu_type pdu_type,
    w_jbuffer_t *jr, w_stm_t stm, json_t *json);

#define BSER_MAGIC "\x00\x01"
#define BSER_V2_MAGIC "\x00\x02"
int w_bser_write_pdu(const uint32_t bser_version, const uint32_t capabilities,
    json_dump_callback_t dump, json_t *json, void *data);
int w_bser_dump(const bser_ctx_t* ctx, json_t *json, void *data);
bool bunser_int(const char *buf, json_int_t avail,
    json_int_t *needed, json_int_t *val);
json_t *bunser(const char *buf, const char *end,
    json_int_t *needed, json_error_t *jerr);

struct watchman_client_response {
  struct watchman_client_response *next;
  json_t *json;
};

struct watchman_client_subscription;

struct watchman_client_state_assertion {
  w_root_t *root; // Holds a ref on the root
  w_string_t *name;
  long id;
};

#include "watchman_perf.h"

struct watchman_client {
  w_stm_t stm;
  w_evt_t ping;
  int log_level;
  w_jbuffer_t reader, writer;
  bool client_mode;
  bool client_is_owner;
  enum w_pdu_type pdu_type;

  // The command currently being processed by dispatch_command
  json_t *current_command;
  w_perf_t perf_sample;

  // This handle is not joinable (CREATE_DETACHED), but can be
  // used to deliver signals.
  pthread_t thread_handle;

  struct watchman_client_response *head, *tail;
};

// These are approximations for managing derived client "classes"
extern void derived_client_dtor(struct watchman_client *client);
extern void derived_client_ctor(struct watchman_client *client);
extern const uint32_t derived_client_size;

// Represents the server side session maintained for a client of
// the watchman per-user process
struct watchman_user_client {
  struct watchman_client client;

  /* map of subscription name => struct watchman_client_subscription */
  w_ht_t *subscriptions;

  /* map of unique id => watchman_client_state_assertion */
  w_ht_t *states;
  long next_state_id;
};

extern pthread_mutex_t w_client_lock;
extern w_ht_t *clients;
void w_client_lock_init(void);

void w_client_vacate_states(struct watchman_user_client *client);

void w_mark_dead(pid_t pid);
bool w_reap_children(bool block);
void w_start_reaper(void);
bool w_is_stopping(void);
extern pthread_t reaper_thread;

void w_request_shutdown(void);

bool w_is_ignored(w_root_t *root, const char *path, uint32_t pathlen);
void w_timeoutms_to_abs_timespec(int timeoutms, struct timespec *deadline);

// Returns the name of the filesystem for the specified path
w_string_t *w_fstype(const char *path);

void w_root_crawl_recursive(w_root_t *root, w_string_t *dir_name, time_t now);
w_root_t *w_root_resolve(const char *path, bool auto_watch, char **errmsg);
w_root_t *w_root_resolve_for_client_mode(const char *filename, char **errmsg);
char *w_find_enclosing_root(const char *filename, char **relpath);
struct watchman_file *w_root_resolve_file(w_root_t *root,
    struct watchman_dir *dir, w_string_t *file_name,
    struct timeval now);

void w_root_perform_age_out(w_root_t *root, int min_age);
void w_root_free_watched_roots(void);
void w_root_schedule_recrawl(w_root_t *root, const char *why);
bool w_root_cancel(w_root_t *root);
bool w_root_stop_watch(w_root_t *root);
json_t *w_root_stop_watch_all(void);
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, bool recursive);
void w_root_reap(void);
void w_root_delref(w_root_t *root);
void w_root_addref(w_root_t *root);
void w_root_set_warning(w_root_t *root, w_string_t *str);

struct watchman_dir *w_root_resolve_dir(w_root_t *root,
    w_string_t *dir_name, bool create);
void w_root_process_path(w_root_t *root,
    struct watchman_pending_collection *coll, w_string_t *full_path,
    struct timeval now, int flags,
    struct watchman_dir_ent *pre_stat);
bool w_root_process_pending(w_root_t *root,
    struct watchman_pending_collection *coll,
    bool pull_from_root);

void w_root_mark_file_changed(w_root_t *root, struct watchman_file *file,
    struct timeval now);

bool w_root_sync_to_now(w_root_t *root, int timeoutms);

void w_root_lock(w_root_t *root, const char *purpose);
bool w_root_lock_with_timeout(w_root_t *root, const char *purpose,
                              int timeoutms);
void w_root_unlock(w_root_t *root);

/* Bob Jenkins' lookup3.c hash function */
uint32_t w_hash_bytes(const void *key, size_t length, uint32_t initval);

struct watchman_rule_match {
  uint32_t root_number;
  w_string_t *relname;
  bool is_new;
  struct watchman_file *file;
};

enum w_clockspec_tag {
  w_cs_timestamp,
  w_cs_clock,
  w_cs_named_cursor
};

struct w_clockspec {
  enum w_clockspec_tag tag;
  union {
    struct timeval timestamp;
    struct {
      uint64_t start_time;
      int pid;
      uint32_t root_number;
      uint32_t ticks;
    } clock;
    struct {
      w_string_t *cursor;
    } named_cursor;
  };
};

struct w_query_since {
  bool is_timestamp;
  union {
    struct timeval timestamp;
    struct {
      bool is_fresh_instance;
      uint32_t ticks;
    } clock;
  };
};

void w_run_subscription_rules(
    struct watchman_user_client *client,
    struct watchman_client_subscription *sub,
    w_root_t *root);
void w_cancel_subscriptions_for_root(w_root_t *root);

void w_match_results_free(uint32_t num_matches,
    struct watchman_rule_match *matches);

static inline uint32_t next_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

/* compare two timevals and return -1 if a is < b, 0 if a == b,
 * or 1 if b > a */
static inline int w_timeval_compare(struct timeval a, struct timeval b)
{
  if (a.tv_sec < b.tv_sec) {
    return -1;
  }
  if (a.tv_sec > b.tv_sec) {
    return 1;
  }
  if (a.tv_usec < b.tv_usec) {
    return -1;
  }
  if (a.tv_usec > b.tv_usec) {
    return 1;
  }
  return 0;
}

#define WATCHMAN_USEC_IN_SEC 1000000
#define WATCHMAN_NSEC_IN_USEC 1000
#define WATCHMAN_NSEC_IN_SEC (1000 * 1000 * 1000)
#define WATCHMAN_NSEC_IN_MSEC 1000000

#if defined(__APPLE__) || defined(__FreeBSD__) \
 || (defined(__NetBSD__) && (__NetBSD_Version__ < 6099000000))
/* BSD-style subsecond timespec */
#define WATCHMAN_ST_TIMESPEC(type) st_##type##timespec
#else
/* POSIX standard timespec */
#define WATCHMAN_ST_TIMESPEC(type) st_##type##tim
#endif

static inline void w_timeval_add(const struct timeval a,
    const struct timeval b, struct timeval *result)
{
  result->tv_sec = a.tv_sec + b.tv_sec;
  result->tv_usec = a.tv_usec + b.tv_usec;

  if (result->tv_usec > WATCHMAN_USEC_IN_SEC) {
    result->tv_sec++;
    result->tv_usec -= WATCHMAN_USEC_IN_SEC;
  }
}

static inline void w_timeval_sub(const struct timeval a,
    const struct timeval b, struct timeval *result)
{
  result->tv_sec = a.tv_sec - b.tv_sec;
  result->tv_usec = a.tv_usec - b.tv_usec;

  if (result->tv_usec < 0) {
    result->tv_sec--;
    result->tv_usec += WATCHMAN_USEC_IN_SEC;
  }
}

static inline void w_timeval_to_timespec(
    const struct timeval a, struct timespec *ts)
{
  ts->tv_sec = a.tv_sec;
  ts->tv_nsec = a.tv_usec * WATCHMAN_NSEC_IN_USEC;
}

static inline void w_timespec_to_timeval(
    const struct timespec ts, struct timeval *tv) {
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / WATCHMAN_NSEC_IN_USEC;
}

// Convert a timeval to a double that holds the fractional number of seconds
static inline double w_timeval_abs_seconds(struct timeval tv){
  double val = (double)tv.tv_sec;
  val += ((double)tv.tv_usec)/WATCHMAN_USEC_IN_SEC;
  return val;
}

static inline double w_timeval_diff(struct timeval start, struct timeval end)
{
  double s = start.tv_sec + ((double)start.tv_usec)/WATCHMAN_USEC_IN_SEC;
  double e = end.tv_sec + ((double)end.tv_usec)/WATCHMAN_USEC_IN_SEC;

  return e - s;
}

extern const char *watchman_tmp_dir;
extern char *watchman_state_file;
extern int dont_save_state;
bool w_state_save(void);
bool w_state_load(void);
bool w_root_save_state(json_t *state);
bool w_root_load_state(json_t *state);
json_t *w_root_trigger_list_to_json(w_root_t *root);
json_t *w_root_watch_list_to_json(void);

#ifdef __APPLE__
int w_get_listener_socket_from_launchd(void);
#endif
bool w_listener_prep_inetd(void);
bool w_start_listener(const char *socket_path);
void w_check_my_sock(void);
char **w_argv_copy_from_json(json_t *arr, int skip);

w_ht_t *w_envp_make_ht(void);
char **w_envp_make_from_ht(w_ht_t *ht, uint32_t *env_size);
void w_envp_set_cstring(w_ht_t *envht, const char *key, const char *val);
void w_envp_set(w_ht_t *envht, const char *key, w_string_t *val);
void w_envp_set_list(w_ht_t *envht, const char *key, json_t *arr);
void w_envp_set_bool(w_ht_t *envht, const char *key, bool val);
void w_envp_unset(w_ht_t *envht, const char *key);

struct watchman_getopt {
  /* name of long option: --optname */
  const char *optname;
  /* if non-zero, short option character */
  int shortopt;
  /* help text shown in the usage information */
  const char *helptext;
  /* whether we accept an argument */
  enum {
    OPT_NONE,
    REQ_STRING,
    REQ_INT,
  } argtype;
  /* if an argument was provided, *val will be set to
   * point to the option value.
   * Because we only update the option if one was provided
   * by the user, you can safely pre-initialize the val
   * pointer to your choice of default.
   * */
  void *val;

  /* if argtype != OPT_NONE, this is the label used to
   * refer to the argument in the help text.  If left
   * blank, we'll use the string "ARG" as a generic
   * alternative */
  const char *arglabel;

  // Whether this option should be passed to the child
  // when running under the gimli monitor
  int is_daemon;
#define IS_DAEMON 1
#define NOT_DAEMON 0
};

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

bool w_getopt(struct watchman_getopt *opts, int *argcp, char ***argvp,
    char ***daemon_argv);
void usage(struct watchman_getopt *opts, FILE *where);
void print_command_list_for_help(FILE *where);

struct w_clockspec *w_clockspec_new_clock(uint32_t root_number, uint32_t ticks);
struct w_clockspec *w_clockspec_parse(json_t *value);
void w_clockspec_eval(w_root_t *root,
    const struct w_clockspec *spec,
    struct w_query_since *since);
void w_clockspec_free(struct w_clockspec *spec);
void w_clockspec_init(void);

const char *get_sock_name(void);

// Helps write shorter lines
static inline void set_prop(json_t *obj, const char *key, json_t *val)
{
  json_object_set_new_nocheck(obj, key, val);
}

void cfg_shutdown(void);
void cfg_set_arg(const char *name, json_t *val);
void cfg_load_global_config_file(void);
json_t *cfg_get_json(w_root_t *root, const char *name);
const char *cfg_get_string(w_root_t *root, const char *name,
    const char *defval);
json_int_t cfg_get_int(w_root_t *root, const char *name,
    json_int_t defval);
bool cfg_get_bool(w_root_t *root, const char *name, bool defval);
double cfg_get_double(w_root_t *root, const char *name, double defval);
mode_t cfg_get_perms(w_root_t *root, const char *name, bool write_bits,
                     bool execute_bits);
const char *cfg_get_trouble_url(void);
json_t *cfg_compute_root_files(bool *enforcing);

#include "watchman_query.h"
#include "watchman_cmd.h"
struct watchman_client_subscription {
  w_root_t *root;
  w_string_t *name;
  w_query *query;
  bool vcs_defer;
  uint32_t last_sub_tick;
  struct w_query_field_list field_list;
  // map of statename => bool.  If true, policy is drop, else defer
  w_ht_t *drop_or_defer;
};

struct watchman_trigger_command {
  w_string_t *triggername;
  w_query *query;
  json_t *definition;
  json_t *command;
  w_ht_t *envht;

  struct w_query_field_list field_list;
  int append_files;
  enum {
    input_dev_null,
    input_json,
    input_name_list
  } stdin_style;
  uint32_t max_files_stdin;

  int stdout_flags;
  int stderr_flags;
  const char *stdout_name;
  const char *stderr_name;

  /* While we are running, this holds the pid
   * of the running process */
  pid_t current_proc;
};

void w_trigger_command_free(struct watchman_trigger_command *cmd);
void w_assess_trigger(w_root_t *root, struct watchman_trigger_command *cmd);
struct watchman_trigger_command *w_build_trigger_from_def(
  w_root_t *root, json_t *trig, char **errmsg);

void set_poison_state(w_root_t *root, w_string_t *dir,
    struct timeval now, const char *syscall, int err,
    const char *reason);

void watchman_watcher_init(void);
void handle_open_errno(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, const char *syscall, int err,
    const char *reason);
void stop_watching_dir(w_root_t *root, struct watchman_dir *dir);
uint32_t strlen_uint32(const char *str);
int w_lstat(const char *path, struct stat *st, bool case_sensitive);

extern struct watchman_ops fsevents_watcher;
extern struct watchman_ops kqueue_watcher;
extern struct watchman_ops inotify_watcher;
extern struct watchman_ops portfs_watcher;
extern struct watchman_ops win32_watcher;

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
#endif

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
