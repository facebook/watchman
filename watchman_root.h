/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <condition_variable>
#include <unordered_map>
#include "CookieSync.h"
#include "QueryableView.h"
#include "watchman_config.h"
#include "watchman_shared_mutex.h"
#include "watchman_synchronized.h"

#define HINT_NUM_DIRS 128*1024
#define CFG_HINT_NUM_DIRS "hint_num_dirs"

#define DEFAULT_SETTLE_PERIOD 20
#define DEFAULT_QUERY_SYNC_MS 60000

/* Prune out nodes that were deleted roughly 12-36 hours ago */
#define DEFAULT_GC_AGE (86400/2)
#define DEFAULT_GC_INTERVAL 86400

/* Idle out watches that haven't had activity in several days */
#define DEFAULT_REAP_AGE (86400*5)

struct watchman_client_state_assertion;

struct watchman_root {
  std::atomic<long> refcnt{1};

  /* path to root */
  w_string root_path;
  bool case_sensitive{false};

  /* our locking granularity is per-root */
  pthread_rwlock_t lock;
  const char *lock_reason{nullptr};

  /* map of rule id => struct watchman_trigger_command */
  watchman::Synchronized<
      std::unordered_map<w_string, std::unique_ptr<watchman_trigger_command>>>
      triggers;

  watchman::CookieSync cookies;

  struct watchman_ignore ignore;

  int trigger_settle{0};
  int gc_interval{0};
  int gc_age{0};
  int idle_reap_age{0};

  /* config options loaded via json file */
  json_ref config_file;
  Configuration config;

  struct RecrawlInfo {
    /* how many times we've had to recrawl */
    int recrawlCount{0};
    w_string lastRecrawlReason;
    /* if true, we've decided that we should re-crawl the root
     * for the sake of ensuring consistency */
    bool shouldRecrawl{false};
    // Last ad-hoc warning message
    w_string warning;
  };
  watchman::Synchronized<RecrawlInfo> recrawlInfo;

  // Why we failed to watch
  w_string failure_reason;

  // map of state name => watchman_client_state_assertion for
  // asserted states
  watchman::Synchronized<std::unordered_map<
      w_string,
      std::unique_ptr<watchman_client_state_assertion>>>
      asserted_states;

  /* --- everything in inner will be reset on w_root_init --- */
  struct Inner {
    /* root number */
    uint32_t number{0};

    /* the watcher that we're using for this root */
    std::shared_ptr<Watcher> watcher;

    std::shared_ptr<watchman::QueryableView> view;

    /* current tick */
    uint32_t ticks{1};

    bool done_initial{0};
    bool cancelled{0};

    /* map of cursor name => last observed tick value */
    watchman::Synchronized<std::unordered_map<w_string, uint32_t>> cursors;

    /* Collection of symlink targets that we try to watch.
     * Reads and writes on this collection are only safe if done from the IO
     * thread; this collection is not protected by the root lock. */
    PendingCollection pending_symlink_targets;

    uint32_t next_cmd_id{0};

    // This is only read or written by process_triggers
    uint32_t last_trigger_tick{0};

    time_t last_cmd_timestamp{0};
    time_t last_reap_timestamp{0};

    explicit Inner(
        const w_string& root_path,
        watchman::CookieSync& cookies,
        Configuration& config);
    ~Inner();
  } inner;

  explicit watchman_root(const w_string& root_path);
  ~watchman_root();

  void considerAgeOut();
  void performAgeOut(std::chrono::seconds min_age);
};

struct write_locked_watchman_root {
  w_root_t *root;
};

struct read_locked_watchman_root {
  const w_root_t *root;
};

/** Massage a write lock into a read lock.
 * This is suitable for passing a write lock to functions that want a
 * read lock.  It works by simply casting the address of the write lock
 * pointer to a read lock pointer.  Casting in this direction is fine,
 * but not casting in the opposite direction.
 * This is safe for a couple of reasons:
 *
 *  1. The read and write lock holders are binary compatible with each
 *     other; they both simply hold a root pointer.
 *  2. The underlying unlock function pthread_rwlock_unlock works regardless
 *     of the read-ness or write-ness of the lock, even though we have
 *     a separate read and write unlock functions.
 *  3. We're careful to pass a pointer to the existing lock instance
 *     around rather than copying it around; that way don't lose track of
 *     the fact that we unlocked the root.
 */
static inline struct read_locked_watchman_root *
w_root_read_lock_from_write(struct write_locked_watchman_root *lock) {
  return (struct read_locked_watchman_root*)lock;
}

struct unlocked_watchman_root {
  w_root_t *root;
};

bool w_root_resolve(
    const char* path,
    bool auto_watch,
    char** errmsg,
    struct unlocked_watchman_root* unlocked);
bool w_root_resolve_for_client_mode(
    const char* filename,
    char** errmsg,
    struct unlocked_watchman_root* unlocked);
char* w_find_enclosing_root(const char* filename, char** relpath);

void w_root_free_watched_roots(void);
void w_root_schedule_recrawl(w_root_t* root, const char* why);
bool w_root_cancel(w_root_t* root);
bool w_root_stop_watch(struct unlocked_watchman_root* unlocked);
json_ref w_root_stop_watch_all(void);
void w_root_reap(void);
void w_root_delref(struct unlocked_watchman_root* unlocked);
void w_root_delref_raw(w_root_t* root);
void w_root_addref(w_root_t* root);

bool w_root_sync_to_now(struct unlocked_watchman_root* unlocked, int timeoutms);

void w_root_lock(
    struct unlocked_watchman_root* unlocked,
    const char* purpose,
    struct write_locked_watchman_root* locked);
bool w_root_lock_with_timeout(
    struct unlocked_watchman_root* unlocked,
    const char* purpose,
    int timeoutms,
    struct write_locked_watchman_root* locked);
void w_root_unlock(
    struct write_locked_watchman_root* locked,
    struct unlocked_watchman_root* unlocked);

void w_root_read_lock(
    struct unlocked_watchman_root* unlocked,
    const char* purpose,
    struct read_locked_watchman_root* locked);
bool w_root_read_lock_with_timeout(
    struct unlocked_watchman_root* unlocked,
    const char* purpose,
    int timeoutms,
    struct read_locked_watchman_root* locked);
void w_root_read_unlock(
    struct read_locked_watchman_root* locked,
    struct unlocked_watchman_root* unlocked);

void process_pending_symlink_targets(struct unlocked_watchman_root* unlocked);
void* run_io_thread(void* arg);
void* run_notify_thread(void* arg);
void process_subscriptions(struct read_locked_watchman_root* lock);
void process_triggers(struct read_locked_watchman_root* lock);
bool consider_reap(struct read_locked_watchman_root* lock);
void remove_from_file_list(struct watchman_file* file);
void free_file_node(struct watchman_file* file);
void w_root_teardown(w_root_t* root);
bool w_root_init(w_root_t* root, char** errmsg);
bool remove_root_from_watched(
    w_root_t* root /* don't care about locked state */);
bool is_vcs_op_in_progress(struct read_locked_watchman_root* lock);
extern const struct watchman_hash_funcs dirname_hash_funcs;
void delete_dir(struct watchman_dir* dir);
bool did_file_change(struct watchman_stat *saved, struct watchman_stat *fresh);
void struct_stat_to_watchman_stat(const struct stat *st,
                                  struct watchman_stat *target);
bool apply_ignore_vcs_configuration(w_root_t *root, char **errmsg);
w_root_t *w_root_new(const char *path, char **errmsg);
extern std::atomic<long> live_roots;
bool root_start(w_root_t *root, char **errmsg);

extern watchman::Synchronized<std::unordered_map<w_string, w_root_t*>>
    watched_roots;

bool root_resolve(
    const char* filename,
    bool auto_watch,
    bool* created,
    char** errmsg,
    struct unlocked_watchman_root* unlocked);
void signal_root_threads(w_root_t* root);

void set_poison_state(
    const w_string& dir,
    struct timeval now,
    const char* syscall,
    int err,
    const char* reason);

void handle_open_errno(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* syscall,
    int err,
    const char* reason);
