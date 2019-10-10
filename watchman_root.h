/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <folly/Synchronized.h>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include "CookieSync.h"
#include "FileSystem.h"
#include "PubSub.h"
#include "QueryableView.h"
#include "watchman_config.h"

#define HINT_NUM_DIRS 128 * 1024
#define CFG_HINT_NUM_DIRS "hint_num_dirs"

#define DEFAULT_SETTLE_PERIOD 20
constexpr std::chrono::milliseconds DEFAULT_QUERY_SYNC_MS(60000);

/* Prune out nodes that were deleted roughly 12-36 hours ago */
#define DEFAULT_GC_AGE (86400 / 2)
#define DEFAULT_GC_INTERVAL 86400

/* Idle out watches that haven't had activity in several days */
#define DEFAULT_REAP_AGE (86400 * 5)

namespace watchman {
class ClientStateAssertion;

class ClientStateAssertions {
 public:
  /** Returns true if `assertion` is the front instance in the queue
   * of assertions that match assertion->name */
  bool isFront(const std::shared_ptr<ClientStateAssertion>& assertion) const;

  /** Returns true if `assertion` currently has an Asserted disposition */
  bool isStateAsserted(w_string stateName) const;

  /** Add assertion to the queue of assertions for assertion->name.
   * Throws if the named state is already asserted or if there is
   * a pending assertion for that state. */
  void queueAssertion(std::shared_ptr<ClientStateAssertion> assertion);

  /** remove assertion from the queue of assertions for assertion->name.
   * If no more assertions remain in that named queue then the queue is
   * removed.
   * If the removal of an assertion causes the new front of that queue
   * to occupied by an assertion with Asserted disposition, generates a
   * broadcast of its enterPayload.
   */
  bool removeAssertion(const std::shared_ptr<ClientStateAssertion>& assertion);

  /** Returns some diagnostic information that is used by
   * the integration tests. */
  json_ref debugStates() const;

 private:
  /** states_ maps from a state name to a queue of assertions with
   * various dispositions */
  std::
      unordered_map<w_string, std::deque<std::shared_ptr<ClientStateAssertion>>>
          states_;
};
}; // namespace watchman

struct watchman_root : public std::enable_shared_from_this<watchman_root> {
  /* path to root */
  w_string root_path;
  /* filesystem type name, as returned by w_fstype() */
  const w_string fs_type;
  watchman::CaseSensitivity case_sensitive;

  /* map of rule id => struct watchman_trigger_command */
  folly::Synchronized<
      std::unordered_map<w_string, std::unique_ptr<watchman_trigger_command>>>
      triggers;

  watchman::CookieSync cookies;

  struct watchman_ignore ignore;

  /* config options loaded via json file */
  json_ref config_file;
  Configuration config;

  const int trigger_settle{0};
  const int gc_interval{0};
  const int gc_age{0};
  const int idle_reap_age{0};

  // Stream of broadcast unilateral items emitted by this root
  std::shared_ptr<watchman::Publisher> unilateralResponses;

  struct RecrawlInfo {
    /* how many times we've had to recrawl */
    int recrawlCount{0};
    /* if true, we've decided that we should re-crawl the root
     * for the sake of ensuring consistency */
    bool shouldRecrawl{true};
    // Last ad-hoc warning message
    w_string warning;
  };
  folly::Synchronized<RecrawlInfo> recrawlInfo;

  // Why we failed to watch
  w_string failure_reason;

  // State transition counter to allow identification of concurrent state
  // transitions
  std::atomic<uint32_t> stateTransCount{0};
  folly::Synchronized<watchman::ClientStateAssertions> assertedStates;

  struct Inner {
    std::shared_ptr<watchman::QueryableView> view_;

    bool done_initial{0};
    bool cancelled{0};

    /* map of cursor name => last observed tick value */
    folly::Synchronized<std::unordered_map<w_string, uint32_t>> cursors;

    /* Collection of symlink targets that we try to watch.
     * Reads and writes on this collection are only safe if done from the IO
     * thread; this collection is not protected by the root lock. */
    PendingCollection pending_symlink_targets;

    time_t last_cmd_timestamp{0};
    mutable time_t last_reap_timestamp{0};

    void init(w_root_t* root);
  } inner;

  // Obtain the current view pointer.
  // This is safe wrt. a concurrent recrawl operation
  std::shared_ptr<watchman::QueryableView> view();

  explicit watchman_root(const w_string& root_path, const w_string& fs_type);
  ~watchman_root();

  void considerAgeOut();
  void performAgeOut(std::chrono::seconds min_age);
  void syncToNow(std::chrono::milliseconds timeout);
  void scheduleRecrawl(const char* why);

  // Requests cancellation of the root.
  // Returns true if this request caused the root cancellation, false
  // if it was already in the process of being cancelled.
  bool cancel();

  void processPendingSymlinkTargets();

  // Returns true if the caller should stop the watch.
  bool considerReap() const;
  void init();
  bool removeFromWatched();
  void applyIgnoreVCSConfiguration();
  void signalThreads();
  bool stopWatch();
  json_ref triggerListToJson() const;

 private:
  void applyIgnoreConfiguration();
};

std::shared_ptr<w_root_t>
w_root_resolve(const char* path, bool auto_watch, char** errmsg);

std::shared_ptr<w_root_t> w_root_resolve_for_client_mode(
    const char* filename,
    char** errmsg);
bool findEnclosingRoot(
    const w_string& fileName,
    w_string_piece& prefix,
    w_string_piece& relativePath);

void w_root_free_watched_roots(void);
json_ref w_root_stop_watch_all(void);
void w_root_reap(void);

bool did_file_change(
    const watchman::FileInformation* saved,
    const watchman::FileInformation* fresh);
extern std::atomic<long> live_roots;

extern folly::Synchronized<
    std::unordered_map<w_string, std::shared_ptr<w_root_t>>>
    watched_roots;

std::shared_ptr<w_root_t> root_resolve(
    const char* filename,
    bool auto_watch,
    bool* created,
    char** errmsg);

void set_poison_state(
    const w_string& dir,
    struct timeval now,
    const char* syscall,
    const std::error_code& err);

void handle_open_errno(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* syscall,
    const std::error_code& err);
