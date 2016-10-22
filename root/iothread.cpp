/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {
void InMemoryView::fullCrawl(
    unlocked_watchman_root* unlocked,
    watchman_pending_collection& pending) {
  struct timeval start;
  struct write_locked_watchman_root lock;

  w_perf_t sample("full-crawl");
  if (config_.getBool("iothrottle", false)) {
    w_ioprio_set_low();
  }
  w_root_lock(unlocked, "io_thread: bump ticks", &lock);
  // Ensure that we observe these files with a new, distinct clock,
  // otherwise a fresh subscription established immediately after a watch
  // can get stuck with an empty view until another change is observed
  lock.root->inner.ticks++;
  gettimeofday(&start, NULL);
  w_pending_coll_add(&pending_, lock.root->root_path, start, 0);
  // There is the potential for a subtle race condition here.  The boolean
  // parameter indicates whether we want to merge in the set of
  // notifications pending from the watcher or not.  Since we now coalesce
  // overlaps we must consume our outstanding set before we merge in any
  // new kernel notification information or we risk missing out on
  // observing changes that happen during the initial crawl.  This
  // translates to a two level loop; the outer loop sweeps in data from
  // inotify, then the inner loop processes it and any dirs that we pick up
  // from recursive processing.
  while (processPending(&lock, &pending, true)) {
    while (processPending(&lock, &pending, false)) {
      ;
    }
  }
  lock.root->inner.done_initial = true;
  sample.add_root_meta(lock.root);
  w_root_unlock(&lock, unlocked);

  if (config_.getBool("iothrottle", false)) {
    w_ioprio_set_normal();
  }

  sample.finish();
  sample.force_log();
  sample.log();

  w_log(
      W_LOG_ERR,
      "%scrawl complete\n",
      unlocked->root->recrawlInfo.rlock()->recrawlCount ? "re" : "");
}

// Performs settle-time actions.
// Returns true if the root was reaped and the io thread should terminate.
static bool do_settle_things(struct unlocked_watchman_root* unlocked) {
  struct write_locked_watchman_root lock;

  // No new pending items were given to us, so consider that
  // we may now be settled.

  process_pending_symlink_targets(unlocked);

  w_root_lock(unlocked, "io_thread: settle out", &lock);
  if (!lock.root->inner.done_initial) {
    // we need to recrawl, stop what we're doing here
    w_root_unlock(&lock, unlocked);
    return false;
  }

  process_subscriptions(w_root_read_lock_from_write(&lock));
  process_triggers(w_root_read_lock_from_write(&lock));
  if (consider_reap(w_root_read_lock_from_write(&lock))) {
    w_root_unlock(&lock, unlocked);
    w_root_stop_watch(unlocked);
    return true;
  }

  lock.root->considerAgeOut();
  w_root_unlock(&lock, unlocked);
  return false;
}

void InMemoryView::clientModeCrawl(unlocked_watchman_root* unlocked) {
  struct watchman_pending_collection pending;

  fullCrawl(unlocked, pending);
}

void InMemoryView::ioThread(unlocked_watchman_root* unlocked) {
  int timeoutms, biggest_timeout;
  struct watchman_pending_collection pending;
  struct write_locked_watchman_root lock;

  timeoutms = unlocked->root->trigger_settle;

  // Upper bound on sleep delay.  These options are measured in seconds.
  biggest_timeout = unlocked->root->gc_interval;
  if (biggest_timeout == 0 ||
      (unlocked->root->idle_reap_age != 0 &&
       unlocked->root->idle_reap_age < biggest_timeout)) {
    biggest_timeout = unlocked->root->idle_reap_age;
  }
  if (biggest_timeout == 0) {
    biggest_timeout = 86400;
  }
  // And convert to milliseconds
  biggest_timeout *= 1000;

  while (!stopThreads_) {
    bool pinged;

    if (!unlocked->root->inner.done_initial) {
      /* first order of business is to find all the files under our root */
      fullCrawl(unlocked, pending);

      timeoutms = unlocked->root->trigger_settle;
    }

    // Wait for the notify thread to give us pending items, or for
    // the settle period to expire
    w_log(W_LOG_DBG, "poll_events timeout=%dms\n", timeoutms);
    pinged = w_pending_coll_lock_and_wait(&pending_, timeoutms);
    w_log(W_LOG_DBG, " ... wake up (pinged=%s)\n", pinged ? "true" : "false");
    w_pending_coll_append(&pending, &pending_);
    w_pending_coll_unlock(&pending_);

    if (!pinged && w_pending_coll_size(&pending) == 0) {
      if (do_settle_things(unlocked)) {
        break;
      }
      timeoutms = MIN(biggest_timeout, timeoutms * 2);
      continue;
    }

    // Otherwise we have pending items to stat and crawl

    // We are now, by definition, unsettled, so reduce sleep timeout
    // to the settle duration ready for the next loop through
    timeoutms = unlocked->root->trigger_settle;

    w_root_lock(unlocked, "io_thread: process notifications", &lock);
    if (!lock.root->inner.done_initial) {
      // we need to recrawl.  Discard these notifications
      w_pending_coll_drain(&pending);
      w_root_unlock(&lock, unlocked);
      continue;
    }

    lock.root->inner.ticks++;
    // If we're not settled, we need an opportunity to age out
    // dead file nodes.  This happens in the test harness.
    lock.root->considerAgeOut();

    while (processPending(&lock, &pending, false)) {
      ;
    }

    w_root_unlock(&lock, unlocked);
  }
}

void InMemoryView::processPath(
    write_locked_watchman_root* lock,
    struct watchman_pending_collection* coll,
    const w_string& full_path,
    struct timeval now,
    int flags,
    struct watchman_dir_ent* pre_stat) {
  /* From a particular query's point of view, there are four sorts of cookies we
   * can observe:
   * 1. Cookies that this query has created. This marks the end of this query's
   *    sync_to_now, so we hide it from the results.
   * 2. Cookies that another query on the same watch by the same process has
   *    created. This marks the end of that other query's sync_to_now, so from
   *    the point of view of this query we turn a blind eye to it.
   * 3. Cookies created by another process on the same watch. We're independent
   *    of other processes, so we report these.
   * 4. Cookies created by a nested watch by the same or a different process.
   *    We're independent of other watches, so we report these.
   *
   * The below condition is true for cases 1 and 2 and false for 3 and 4.
   */
  if (w_string_startswith(full_path, cookies_.cookiePrefix())) {
    bool consider_cookie = (watcher->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS)
        ? ((flags & W_PENDING_VIA_NOTIFY) || !lock->root->inner.done_initial)
        : true;

    if (consider_cookie) {
      cookies_.notifyCookie(full_path);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(full_path, root_path) ||
      (flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(lock, coll, full_path, now,
        (flags & W_PENDING_RECURSIVE) == W_PENDING_RECURSIVE);
  } else {
    statPath(
        w_root_read_lock_from_write(lock),
        coll,
        full_path,
        now,
        flags,
        pre_stat);
  }
}

bool InMemoryView::processPending(
    write_locked_watchman_root* lock,
    watchman_pending_collection* coll,
    bool pullFromRoot) {
  if (pullFromRoot) {
    // You MUST own root->pending lock for this
    w_pending_coll_append(coll, &pending_);
  }

  if (!coll->pending) {
    return false;
  }

  w_log(
      W_LOG_DBG,
      "processing %d events in %s\n",
      w_pending_coll_size(coll),
      root_path.c_str());

  // Steal the contents
  auto pending = coll->pending;
  coll->pending = NULL;
  w_pending_coll_drain(coll);

  while (pending) {
    auto p = pending;
    pending = p->next;

    if (!stopThreads_) {
      processPath(lock, coll, p->path, p->now, p->flags, nullptr);
    }

    w_pending_fs_free(p);
  }

  return true;
}
}

/* vim:ts=2:sw=2:et:
 */
