/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {

std::shared_future<void> InMemoryView::waitUntilReadyToQuery(
    const std::shared_ptr<w_root_t>& root) {
  auto lockPair = acquireLockedPair(root->recrawlInfo, crawlState_);

  if (lockPair.second->promise && lockPair.second->future.valid()) {
    return lockPair.second->future;
  }

  if (root->inner.done_initial && !lockPair.first->shouldRecrawl) {
    // Return an already satisfied future
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }

  // Not yet done, so queue up the promise
  lockPair.second->promise = watchman::make_unique<std::promise<void>>();
  lockPair.second->future =
      std::shared_future<void>(lockPair.second->promise->get_future());
  return lockPair.second->future;
}

void InMemoryView::fullCrawl(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& pending) {
  struct timeval start;

  w_perf_t sample("full-crawl");
  if (config_.getBool("iothrottle", false)) {
    w_ioprio_set_low();
  }
  {
    auto view = view_.wlock();
    // Ensure that we observe these files with a new, distinct clock,
    // otherwise a fresh subscription established immediately after a watch
    // can get stuck with an empty view until another change is observed
    view->mostRecentTick++;
    gettimeofday(&start, NULL);
    pending_.wlock()->add(root->root_path, start, W_PENDING_RECURSIVE);
    // There is the potential for a subtle race condition here.  The boolean
    // parameter indicates whether we want to merge in the set of
    // notifications pending from the watcher or not.  Since we now coalesce
    // overlaps we must consume our outstanding set before we merge in any
    // new kernel notification information or we risk missing out on
    // observing changes that happen during the initial crawl.  This
    // translates to a two level loop; the outer loop sweeps in data from
    // inotify, then the inner loop processes it and any dirs that we pick up
    // from recursive processing.
    while (processPending(root, view, pending, true)) {
      while (processPending(root, view, pending, false)) {
        ;
      }
    }
    {
      auto lockPair = acquireLockedPair(root->recrawlInfo, crawlState_);
      lockPair.first->shouldRecrawl = false;
      if (lockPair.second->promise) {
        lockPair.second->promise->set_value();
        lockPair.second->promise.reset();
      }
      root->inner.done_initial = true;
    }
    root->cookies.abortAllCookies();
  }
  sample.add_root_meta(root);

  if (config_.getBool("iothrottle", false)) {
    w_ioprio_set_normal();
  }

  sample.finish();
  sample.force_log();
  sample.log();

  w_log(
      W_LOG_ERR,
      "%scrawl complete\n",
      root->recrawlInfo.rlock()->recrawlCount ? "re" : "");
}

// Performs settle-time actions.
// Returns true if the root was reaped and the io thread should terminate.
static bool do_settle_things(const std::shared_ptr<w_root_t>& root) {
  // No new pending items were given to us, so consider that
  // we may now be settled.

  root->processPendingSymlinkTargets();

  if (!root->inner.done_initial) {
    // we need to recrawl, stop what we're doing here
    return false;
  }

  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  w_assert(view, "we're called from InMemoryView, wat?");
  view->warmContentCache();

  auto settledPayload = json_object({{"settled", json_true()}});
  root->unilateralResponses->enqueue(std::move(settledPayload));

  if (root->considerReap()) {
    root->stopWatch();
    return true;
  }

  root->considerAgeOut();
  return false;
}

void InMemoryView::clientModeCrawl(const std::shared_ptr<w_root_t>& root) {
  PendingCollection pending;

  auto lock = pending.wlock();
  fullCrawl(root, lock);
}

bool InMemoryView::handleShouldRecrawl(const std::shared_ptr<w_root_t>& root) {
  {
    auto info = root->recrawlInfo.rlock();
    if (!info->shouldRecrawl) {
      return false;
    }
  }

  if (!root->inner.cancelled) {
    auto info = root->recrawlInfo.wlock();
    info->recrawlCount++;
    root->inner.done_initial = false;
  }

  return true;
}

void InMemoryView::ioThread(const std::shared_ptr<w_root_t>& root) {
  int timeoutms, biggest_timeout;
  PendingCollection pending;
  auto localPendingLock = pending.wlock();

  timeoutms = root->trigger_settle;

  // Upper bound on sleep delay.  These options are measured in seconds.
  biggest_timeout = root->gc_interval;
  if (biggest_timeout == 0 ||
      (root->idle_reap_age != 0 && root->idle_reap_age < biggest_timeout)) {
    biggest_timeout = root->idle_reap_age;
  }
  if (biggest_timeout == 0) {
    biggest_timeout = 86400;
  }
  // And convert to milliseconds
  biggest_timeout *= 1000;

  while (!stopThreads_) {
    bool pinged;

    if (!root->inner.done_initial) {
      /* first order of business is to find all the files under our root */
      fullCrawl(root, localPendingLock);

      timeoutms = root->trigger_settle;
    }

    // Wait for the notify thread to give us pending items, or for
    // the settle period to expire
    {
      w_log(W_LOG_DBG, "poll_events timeout=%dms\n", timeoutms);
      auto targetPendingLock =
          pending_.lockAndWait(std::chrono::milliseconds(timeoutms), pinged);
      w_log(W_LOG_DBG, " ... wake up (pinged=%s)\n", pinged ? "true" : "false");
      localPendingLock->append(&*targetPendingLock);
    }

    if (handleShouldRecrawl(root)) {
      fullCrawl(root, localPendingLock);
      timeoutms = root->trigger_settle;
      continue;
    }

    if (!pinged && localPendingLock->size() == 0) {
      if (do_settle_things(root)) {
        break;
      }
      timeoutms = std::min(biggest_timeout, timeoutms * 2);
      continue;
    }

    // Otherwise we have pending items to stat and crawl

    // We are now, by definition, unsettled, so reduce sleep timeout
    // to the settle duration ready for the next loop through
    timeoutms = root->trigger_settle;

    {
      auto view = view_.wlock();
      if (!root->inner.done_initial) {
        // we need to recrawl.  Discard these notifications
        localPendingLock->drain();
        continue;
      }

      view->mostRecentTick++;

      while (processPending(root, view, localPendingLock, false)) {
        ;
      }
    }
  }
}

void InMemoryView::processPath(
    const std::shared_ptr<w_root_t>& root,
    SyncView::LockedPtr& view,
    PendingCollection::LockedPtr& coll,
    const w_string& full_path,
    struct timeval now,
    int flags,
    const watchman_dir_ent* pre_stat) {
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
    bool consider_cookie =
        (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS)
        ? ((flags & W_PENDING_VIA_NOTIFY) || !root->inner.done_initial)
        : true;

    if (consider_cookie) {
      cookies_.notifyCookie(full_path);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(full_path, root_path) ||
      (flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(
        root,
        view,
        coll,
        full_path,
        now,
        (flags & W_PENDING_RECURSIVE) == W_PENDING_RECURSIVE);
  } else {
    statPath(root, view, coll, full_path, now, flags, pre_stat);
  }
}

bool InMemoryView::processPending(
    const std::shared_ptr<w_root_t>& root,
    SyncView::LockedPtr& view,
    PendingCollection::LockedPtr& coll,
    bool pullFromRoot) {
  if (pullFromRoot) {
    auto srcLock = pending_.wlock();
    coll->append(&*srcLock);
  }

  if (!coll->size()) {
    return false;
  }

  w_log(
      W_LOG_DBG,
      "processing %d events in %s\n",
      coll->size(),
      root_path.c_str());

  auto pending = coll->stealItems();

  while (pending) {
    if (!stopThreads_) {
      processPath(
          root,
          view,
          coll,
          pending->path,
          pending->now,
          pending->flags,
          nullptr);
    }

    pending = std::move(pending->next);
  }

  return true;
}
}

/* vim:ts=2:sw=2:et:
 */
