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
#include <unistd.h>
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
#include <libgen.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#if defined(__linux__) && !defined(O_CLOEXEC)
# define O_CLOEXEC   02000000 /* set close_on_exec, from asm/fcntl.h */
#endif
#ifndef O_CLOEXEC
# define O_CLOEXEC 0
#endif
#include <sys/poll.h>
#include <sys/wait.h>
#include <fnmatch.h>
#ifdef HAVE_PCRE_H
# include <pcre.h>
#endif
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif
#include <spawn.h>
// Not explicitly exported on Darwin, so we get to define it.
extern char **environ;

#include "watchman_hash.h"

#include "jansson.h"

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
#else
# define ignore_result(x) x
#endif

// self-documenting hint to the compiler that we didn't use it
#define unused_parameter(x)  (void)x

static inline void w_refcnt_add(int *refcnt)
{
  (void)__sync_fetch_and_add(refcnt, 1);
}

/* returns true if we deleted the last ref */
static inline bool w_refcnt_del(int *refcnt)
{
  return __sync_add_and_fetch(refcnt, -1) == 0;
}

static inline void w_set_cloexec(int fd)
{
  ignore_result(fcntl(fd, F_SETFD, FD_CLOEXEC));
}

static inline void w_set_nonblock(int fd)
{
  ignore_result(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK));
}

static inline void w_clear_nonblock(int fd)
{
  ignore_result(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK));
}

struct watchman_string;
typedef struct watchman_string w_string_t;
struct watchman_string {
  int refcnt;
  uint32_t hval;
  uint32_t len;
  w_string_t *slice;
  const char *buf;
};

#ifndef IN_EXCL_UNLINK
/* defined in <linux/inotify.h> but we can't include that without
 * breaking userspace */
# define WATCHMAN_IN_EXCL_UNLINK 0x04000000
#else
# define WATCHMAN_IN_EXCL_UNLINK IN_EXCL_UNLINK
#endif

#define WATCHMAN_INOTIFY_MASK \
  IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | \
  IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR | WATCHMAN_IN_EXCL_UNLINK

#define WATCHMAN_PORT_EVENTS \
  FILE_MODIFIED | FILE_ATTRIB | FILE_NOFOLLOW

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_pending_fs;
struct watchman_trigger_command;
typedef struct watchman_root w_root_t;

struct watchman_clock {
  uint32_t ticks;
  struct timeval tv;
};
typedef struct watchman_clock w_clock_t;

struct watchman_pending_fs {
  struct watchman_pending_fs *next;
  w_string_t *path;
  bool recursive;
  struct timeval now;
  bool via_notify;
};

struct watchman_dir {
  /* full path */
  w_string_t *path;
  /* files contained in this dir (keyed by file->name) */
  w_ht_t *files;
  /* child dirs contained in this dir (keyed by dir->path) */
  w_ht_t *dirs;

  /* watch descriptor */
  int wd;
#if HAVE_PORT_CREATE
  file_obj_t port_file;
#endif
};

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
  struct stat st;

#if HAVE_PORT_CREATE
  file_obj_t port_file;
#endif
};

#define WATCHMAN_COOKIE_PREFIX ".watchman-cookie-"
struct watchman_query_cookie {
  pthread_cond_t cond;
  bool seen;
};

#define WATCHMAN_BATCH_LIMIT (16*1024)

struct watchman_root {
#if HAVE_INOTIFY_INIT
  /* we use one inotify instance per watched root dir */
  int infd;

  /* map of active watch descriptor to a dir */
  w_ht_t *wd_to_dir;
#endif
#if HAVE_KQUEUE
  int kq_fd;
#endif
#if HAVE_PORT_CREATE
  int port_fd;
#endif
  int refcnt;

  /* path to root */
  w_string_t *root_path;

  /* map of dir name to a dir */
  w_ht_t *dirname_to_dir;

  /* queue of items that we need to stat/process */
  struct watchman_pending_fs *pending;
  w_ht_t *pending_uniq;

  /* the most recently changed file */
  struct watchman_file *latest_file;

  /* current tick */
  uint32_t ticks;

  bool done_initial;
  /* if true, we've decided that we should re-crawl the root
   * for the sake of ensuring consistency */
  bool should_recrawl;
  bool cancelled;

  /* relative path to the query cookie dir.
   * If NULL, we use the root itself */
  w_string_t *query_cookie_dir;
  w_ht_t *query_cookies;

  /* map of dir name => dirname
   * if the map has an entry for a given dir, we're ignoring it */
  w_ht_t *ignore_dirs;

  /* map of cursor name => last observed tick value */
  w_ht_t *cursors;

  /* map of filename suffix => watchman_file at the head
   * of the suffix index.  Linkage via suffix_next */
  w_ht_t *suffixes;

  /* map of rule id => struct watchman_trigger_command */
  w_ht_t *commands;
  uint32_t next_cmd_id;
  uint32_t last_trigger_tick;
  uint32_t pending_trigger_tick;
  uint32_t last_sub_tick;
  uint32_t pending_sub_tick;

  /* our locking granularity is per-root */
  pthread_mutex_t lock;
  pthread_t notify_thread;
#if HAVE_INOTIFY_INIT
  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf[WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + 256)];
#endif
#if HAVE_KQUEUE
  struct kevent keventbuf[WATCHMAN_BATCH_LIMIT];
#endif
#if HAVE_PORT_CREATE
  port_event_t portevents[WATCHMAN_BATCH_LIMIT];
#endif
};

struct watchman_rule {
  /* if true, include matches in the set of signalled paths,
   * else exclude it */
  bool include;
  /* if true, this rule matches if the pattern doesn't match.
   * otherwise it matches if the pattern matches */
  bool negated;

  enum {
    RT_FNMATCH,
    RT_PCRE,
  } rule_type;

  /* pattern passed to fnmatch(3) */
  const char *pattern;
  /* flags passed to fnmatch(3) */
  int flags;

#ifdef HAVE_PCRE_H
  pcre *re;
  pcre_extra *re_extra;
#endif

  /* next rule in this chain */
  struct watchman_rule *next;
};
void w_free_rules(struct watchman_rule *head);

struct watchman_json_buffer {
  char *buf;
  uint32_t allocd;
  uint32_t rpos, wpos;
};
typedef struct watchman_json_buffer w_jbuffer_t;
bool w_json_buffer_init(w_jbuffer_t *jr);
void w_json_buffer_free(w_jbuffer_t *jr);
json_t *w_json_buffer_next(w_jbuffer_t *jr, int fd, json_error_t *jerr);
bool w_json_buffer_write(w_jbuffer_t *jr, int fd, json_t *json, int flags);

struct watchman_client_response {
  struct watchman_client_response *next, *prev;
  json_t *json;
};

struct watchman_client_subscription;

struct watchman_client {
  int fd;
  int ping[2];
  int log_level;
  w_jbuffer_t reader, writer;
  bool client_mode;

  struct watchman_client_response *head, *tail;
  /* map of subscription name => struct watchman_client_subscription */
  w_ht_t *subscriptions;

  pthread_mutex_t lock;
};
extern pthread_mutex_t w_client_lock;
extern w_ht_t *clients;

struct watchman_trigger_command {
  w_string_t *triggername;
  struct watchman_rule *rules;
  char **argv;
  uint32_t argc;

  /* tick value when we were last assessed
   * for triggers */
  uint32_t dispatch_tick;
  /* While we are running, this holds the pid
   * of the running process */
  pid_t current_proc;
};

void w_mark_dead(pid_t pid);
bool w_reap_children(bool block);

#define W_LOG_OFF 0
#define W_LOG_ERR 1
#define W_LOG_DBG 2
#define W_LOG_FATAL 3

extern int log_level;
void w_log(int level, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
;

void w_log_to_clients(int level, const char *buf);


w_string_t *w_string_new(const char *str);
w_string_t *w_string_new_lower(const char *str);
w_string_t *w_string_dup_lower(w_string_t *str);
w_string_t *w_string_suffix(w_string_t *str);
bool w_string_suffix_match(w_string_t *str, w_string_t *suffix);
w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len);
char *w_string_dup_buf(const w_string_t *str);
void w_string_addref(w_string_t *str);
void w_string_delref(w_string_t *str);
int w_string_compare(const w_string_t *a, const w_string_t *b);
bool w_string_equal(const w_string_t *a, const w_string_t *b);
bool w_string_equal_caseless(const w_string_t *a, const w_string_t *b);
w_string_t *w_string_dirname(w_string_t *str);
w_string_t *w_string_basename(w_string_t *str);
w_string_t *w_string_canon_path(w_string_t *str);
w_string_t *w_string_path_cat(w_string_t *parent, w_string_t *rhs);
bool w_string_is_cookie(w_string_t *str);
void w_root_crawl_recursive(w_root_t *root, w_string_t *dir_name, time_t now);
w_root_t *w_root_resolve(const char *path, bool auto_watch);
w_root_t *w_root_resolve_for_client_mode(const char *filename);
void w_root_free_watched_roots(void);
bool w_root_cancel(w_root_t *root);
bool w_root_stop_watch(w_root_t *root);
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    struct timeval now, bool recursive);
void w_root_reap(void);
void w_root_delref(w_root_t *root);
void w_root_addref(w_root_t *root);

struct watchman_dir *w_root_resolve_dir(w_root_t *root,
    w_string_t *dir_name, bool create);
struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd);
void w_root_process_path(w_root_t *root, w_string_t *full_path,
    struct timeval now, bool recursive, bool via_notify);
bool w_root_process_pending(w_root_t *root, bool drain);

bool w_root_add_pending(w_root_t *root, w_string_t *path,
    bool recursive, struct timeval now, bool via_notify);
bool w_root_add_pending_rel(w_root_t *root, struct watchman_dir *dir,
    const char *name, bool recursive,
    struct timeval now, bool via_notify);
bool w_root_sync_to_now(w_root_t *root, int timeoutms);

void w_root_lock(w_root_t *root);
void w_root_unlock(w_root_t *root);

/* Bob Jenkins' lookup3.c hash function */
uint32_t w_hash_bytes(const void *key, size_t length, uint32_t initval);

struct watchman_rule_match {
  w_string_t *relname;
  bool is_new;
  struct watchman_file *file;
};

struct w_clockspec_query {
  bool is_timestamp;
  struct timeval tv;
  uint32_t ticks;
};


uint32_t w_rules_match(w_root_t *root,
    struct watchman_file *oldest_file,
    struct watchman_rule_match **results,
    struct watchman_rule *head,
    struct w_clockspec_query *since);
void w_run_subscription_rules(
    struct watchman_client *client,
    struct watchman_client_subscription *sub,
    w_root_t *root);

json_t *w_match_results_to_json(
    uint32_t num_matches,
    struct watchman_rule_match *matches);
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

#if defined(__APPLE__) || defined(__FreeBSD__)
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

static inline double w_timeval_diff(struct timeval start, struct timeval end)
{
  double s = start.tv_sec + ((double)start.tv_usec)/WATCHMAN_USEC_IN_SEC;
  double e = end.tv_sec + ((double)end.tv_usec)/WATCHMAN_USEC_IN_SEC;

  return e - s;
}

extern int trigger_settle;
extern int recrawl_period;
extern const char *watchman_tmp_dir;
extern char *watchman_state_file;
extern int dont_save_state;
bool w_state_save(void);
bool w_state_load(void);
bool w_root_save_state(json_t *state);
bool w_root_load_state(json_t *state);
json_t *w_root_trigger_list_to_json(w_root_t *root);
json_t *w_root_watch_list_to_json(void);

bool w_start_listener(const char *socket_path);
char **w_argv_copy_from_json(json_t *arr, int skip);

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
    OPT_STRING,
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

bool w_getopt(struct watchman_getopt *opts, int *argcp, char ***argvp,
    char ***daemon_argv);

bool w_parse_clockspec(w_root_t *root,
    json_t *value,
    struct w_clockspec_query *since,
    bool allow_cursor);

const char *get_sock_name(void);

// Helps write shorter lines
static inline void set_prop(json_t *obj, const char *key, json_t *val)
{
  json_object_set_new_nocheck(obj, key, val);
}

#include "watchman_query.h"
#include "watchman_cmd.h"
struct watchman_client_subscription {
  w_root_t *root;
  w_string_t *name;
  w_query *query;
  struct w_clockspec_query since;
  struct w_query_field_list field_list;
};


#ifdef __cplusplus
}
#endif


#endif

/* vim:ts=2:sw=2:et:
 */

