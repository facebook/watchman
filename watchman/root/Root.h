/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <unordered_map>
#include "watchman/CookieSync.h"
#include "watchman/FileSystem.h"
#include "watchman/watchman_ignore.h"
#include "watchman/watchman_string.h"

/* Prune out nodes that were deleted roughly 12-36 hours ago */
#define DEFAULT_GC_AGE (86400 / 2)
#define DEFAULT_GC_INTERVAL 86400

namespace watchman {

enum ClientStateDisposition {
  PendingEnter,
  Asserted,
  PendingLeave,
  Done,
};

class ClientStateAssertion {
 public:
  const std::shared_ptr<Root> root; // Holds a ref on the root
  const w_string name;
  // locking: You must hold root->assertedStates lock to access this member
  ClientStateDisposition disposition{PendingEnter};

  // Deferred payload to send when this assertion makes it to the front
  // of the queue.
  // locking: You must hold root->assertedStates lock to access this member.
  json_ref enterPayload;

  ClientStateAssertion(const std::shared_ptr<Root>& root, const w_string& name)
      : root(root), name(name) {}
};

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

class Root : public std::enable_shared_from_this<Root> {
 public:
  /* path to root */
  w_string root_path;
  /* filesystem type name, as returned by w_fstype() */
  const w_string fs_type;
  CaseSensitivity case_sensitive;

  /* map of rule id => struct TriggerCommand */
  folly::Synchronized<
      std::unordered_map<w_string, std::unique_ptr<TriggerCommand>>>
      triggers;

  CookieSync cookies;

  watchman_ignore ignore;

  /* config options loaded via json file */
  json_ref config_file;
  Configuration config;

  const int trigger_settle{0};
  /**
   * Don't GC more often than this.
   *
   * If zero, then never age out.
   */
  const std::chrono::seconds gc_interval{DEFAULT_GC_INTERVAL};
  /**
   * When GCing, age out files older than this.
   */
  const std::chrono::seconds gc_age{DEFAULT_GC_AGE};
  const int idle_reap_age{0};

  // Stream of broadcast unilateral items emitted by this root
  std::shared_ptr<Publisher> unilateralResponses;

  struct RecrawlInfo {
    /* how many times we've had to recrawl */
    int recrawlCount{0};
    /* if true, we've decided that we should re-crawl the root
     * for the sake of ensuring consistency */
    bool shouldRecrawl{true};
    // Last ad-hoc warning message
    w_string warning;
    std::chrono::steady_clock::time_point crawlStart;
    std::chrono::steady_clock::time_point crawlFinish;
  };
  folly::Synchronized<RecrawlInfo> recrawlInfo;

  // Why we failed to watch
  w_string failure_reason;

  // State transition counter to allow identification of concurrent state
  // transitions
  std::atomic<uint32_t> stateTransCount{0};
  folly::Synchronized<ClientStateAssertions> assertedStates;

  struct Inner {
    std::shared_ptr<QueryableView> view_;

    /**
     * Initially false and set to false by the iothread after scheduleRecrawl.
     * Set true after fullCrawl is done.
     *
     * Primarily used by the iothread but this is atomic because other threads
     * sometimes read it to produce log messages.
     */
    std::atomic<bool> done_initial{false};
    bool cancelled{false};

    /* map of cursor name => last observed tick value */
    folly::Synchronized<std::unordered_map<w_string, uint32_t>> cursors;

    /* Collection of symlink targets that we try to watch.
     * Reads and writes on this collection are only safe if done from the IO
     * thread; this collection is not protected by the root lock. */
    PendingCollection pending_symlink_targets;

    /// Set by connection threads and read on the iothread.
    std::atomic<std::chrono::steady_clock::time_point> last_cmd_timestamp{
        std::chrono::steady_clock::time_point{}};

    /// Only accessed on the iothread.
    std::chrono::steady_clock::time_point last_reap_timestamp;

    void init(Root* root);
  } inner;

  // For debugging and diagnostic purposes, this set references
  // all outstanding query contexts that are executing against this root.
  // If is only safe to read the query contexts while the queries.rlock()
  // is held, and even then it is only really safe to read fields that
  // are not changed by the query exection.
  folly::Synchronized<std::unordered_set<QueryContext*>> queries;

  /**
   * Obtain the current view pointer.
   * This is safe wrt. a concurrent recrawl operation
   */
  std::shared_ptr<QueryableView> view() const;

  Root(const w_string& root_path, const w_string& fs_type);
  ~Root();

  void considerAgeOut();
  void performAgeOut(std::chrono::seconds min_age);
  void syncToNow(
      std::chrono::milliseconds timeout,
      std::vector<w_string>& cookieFileNames);
  void scheduleRecrawl(const char* why);
  void recrawlTriggered(const char* why);

  // Requests cancellation of the root.
  // Returns true if this request caused the root cancellation, false
  // if it was already in the process of being cancelled.
  bool cancel();

  void processPendingSymlinkTargets();

  // Returns true if the caller should stop the watch.
  bool considerReap();
  void init();
  bool removeFromWatched();
  void applyIgnoreVCSConfiguration();
  void signalThreads();
  bool stopWatch();
  json_ref triggerListToJson() const;

  static json_ref getStatusForAllRoots();
  json_ref getStatus() const;

  // Annotate the sample with some standard metadata taken from a root.
  void addPerfSampleMetadata(PerfSample& sample) const;

 private:
  void applyIgnoreConfiguration();
};

} // namespace watchman
