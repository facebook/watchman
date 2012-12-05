/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WATCHMAN_H
#define WATCHMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define _GNU_SOURCE 1
#include "config.h"

#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#if HAVE_SYS_INOTIFY_H
# include <sys/inotify.h>
#endif
#if HAVE_SYS_EVENT_H
# include <sys/event.h>
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
#include <sys/poll.h>

#include "watchman_hash.h"

#include "jansson.h"

/* sane, reasonably large filename size that we'll use
 * throughout; POSIX seems to define smallish buffers
 * that seem risky */
#define WATCHMAN_NAME_MAX   1024

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
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static inline void w_set_nonblock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static inline void w_clear_nonblock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
}

/* we use interned strings to reduce memory bloat */
struct watchman_string;
typedef struct watchman_string w_string_t;
struct watchman_string {
  int refcnt;
  uint32_t hval;
  uint32_t len;
  w_string_t *slice;
  const char *buf;
};

#define WATCHMAN_INOTIFY_MASK \
  IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | \
  IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR

struct watchman_file;
struct watchman_dir;
struct watchman_root;
struct watchman_pending_fs;
struct watchman_trigger_command;
typedef struct watchman_root w_root_t;

struct watchman_clock {
  uint32_t ticks;
  uint32_t seconds;
};
typedef struct watchman_clock w_clock_t;

struct watchman_pending_fs {
  struct watchman_pending_fs *next;
  w_string_t *path;
  bool confident;
  time_t now;
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
};

struct watchman_file {
  /* our name within the parent dir */
  w_string_t *name;
  /* the parent dir */
  struct watchman_dir *parent;

  /* linkage to files ordered by changed time */
  struct watchman_file *prev, *next;

  /* the time we last observed a change to this file */
  w_clock_t otime;

  /* confidence indicator.  We set this if we believe
   * that the file was changed based on the kernel telling
   * us that it did so.  If we had to assume that the file
   * changed (perhaps because of an overflow) then set this
   * to false.  We can expose this data to clients and they
   * can elect to perform more thorough change checks */
  bool confident;

  /* whether we believe that this file still exists */
  bool exists;

  /* cache stat results so we can tell if an entry
   * changed */
  struct stat st;

#if HAVE_KQUEUE
  /* open descriptor on the file so we can monitor it.
   * It sucks that we need one descriptor per filesystem
   * entry, but on the plus side, we don't need to
   * maintain a mapping of descriptor to structs as
   * kqueue will do that for us */
  int kq_fd;
#endif
};

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

  /* path to root */
  w_string_t *root_path;

  /* map of dir name to a dir */
  w_ht_t *dirname_to_dir;

  /* queue of items that we need to stat/process */
  struct watchman_pending_fs *pending;

  /* the most recently changed file */
  struct watchman_file *latest_file;

  /* current tick */
  uint32_t ticks;

  bool done_initial;

  /* map of cursor name => last observed tick value */
  w_ht_t *cursors;

  /* map of rule id => struct watchman_trigger_command */
  w_ht_t *commands;
  uint32_t next_cmd_id;
  uint32_t last_trigger_tick;
  uint32_t pending_trigger_tick;

  /* our locking granularity is per-root */
  pthread_mutex_t lock;
  pthread_cond_t cond;
};

struct watchman_rule {
  /* if true, include matches in the set of signalled paths,
   * else exclude it */
  bool include;
  /* if true, this rule matches if the pattern doesn't match.
   * otherwise it matches if the pattern matches */
  bool negated;
  /* pattern passed to fnmatch(3) */
  const char *pattern;
  /* flags passed to fnmatch(3) */
  int flags;

  /* next rule in this chain */
  struct watchman_rule *next;
};

struct watchman_json_reader {
  char *buf;
  uint32_t allocd;
  uint32_t rpos, wpos;
};
typedef struct watchman_json_reader w_jreader_t;
bool w_json_reader_init(w_jreader_t *jr);
void w_json_reader_free(w_jreader_t *jr);
json_t *w_json_reader_next(w_jreader_t *jr, int fd, json_error_t *jerr);

struct watchman_client_response {
  struct watchman_client_response *next, *prev;
  json_t *json;
};

struct watchman_client {
  int fd;
  int ping[2];
  int log_level;
  w_jreader_t reader;

  struct watchman_client_response *head, *tail;

  pthread_mutex_t lock;
};

struct watchman_trigger_command {
  uint32_t triggerid;
  struct watchman_rule *rules;
  char **argv;
  int argc;
};

#define W_LOG_OFF 0
#define W_LOG_ERR 1
#define W_LOG_DBG 2

void w_log(int level, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
;

void w_log_to_clients(int level, const char *buf);


w_string_t *w_string_new(const char *str);
w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len);
char *w_string_dup_buf(const w_string_t *str);
void w_string_addref(w_string_t *str);
void w_string_delref(w_string_t *str);
int w_string_compare(const w_string_t *a, const w_string_t *b);
bool w_string_equal(const w_string_t *a, const w_string_t *b);
void w_string_collect(void);
w_string_t *w_string_dirname(w_string_t *str);
w_string_t *w_string_basename(w_string_t *str);
w_string_t *w_string_path_cat(const w_string_t *parent, const w_string_t *rhs);
void w_root_crawl_recursive(w_root_t *root, w_string_t *dir_name, time_t now);
w_root_t *w_root_new(const char *path);
w_root_t *w_root_resolve(const char *path, bool auto_watch);
void w_root_mark_deleted(w_root_t *root, struct watchman_dir *dir,
    time_t now, bool confident, bool recursive);

struct watchman_dir *w_root_resolve_dir_by_wd(w_root_t *root, int wd);
void w_root_process_path(w_root_t *root, w_string_t *full_path,
    time_t now, bool confident);
bool w_root_process_pending(w_root_t *root);

bool w_root_add_pending(w_root_t *root, w_string_t *path,
    bool confident, time_t now);
bool w_root_add_pending_rel(w_root_t *root, struct watchman_dir *dir,
    const char *name, bool confident, time_t now);

void w_root_lock(w_root_t *root);
void w_root_unlock(w_root_t *root);

/* Bob Jenkins' lookup3.c hash function */
uint32_t w_hash_bytes(const void *key, size_t length, uint32_t initval);

uint32_t w_rules_match(w_root_t *root,
    struct watchman_file *oldest_file,
    w_ht_t *uniq, struct watchman_rule *head);

static inline uint32_t next_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

static inline double time_diff(struct timeval start, struct timeval end)
{
  double s = start.tv_sec + ((double)start.tv_usec)/1000000;
  double e = end.tv_sec + ((double)end.tv_usec)/1000000;

  return e - s;
}

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
};

bool w_getopt(struct watchman_getopt *opts, int *argcp, char ***argvp);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

