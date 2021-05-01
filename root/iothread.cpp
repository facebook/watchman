/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"

namespace watchman {

std::shared_future<void> InMemoryView::waitUntilReadyToQuery(
    const std::shared_ptr<watchman_root>& root) {
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
  lockPair.second->promise = std::make_unique<std::promise<void>>();
  lockPair.second->future =
      std::shared_future<void>(lockPair.second->promise->get_future());
  return lockPair.second->future;
}

void InMemoryView::fullCrawl(
    const std::shared_ptr<watchman_root>& root,
    PendingChanges& pending) {
  struct timeval start;

  {
    auto crawl = root->recrawlInfo.wlock();
    crawl->crawlStart = std::chrono::steady_clock::now();
  }

  w_perf_t sample("full-crawl");
  {
    auto view = view_.wlock();
    // Ensure that we observe these files with a new, distinct clock,
    // otherwise a fresh subscription established immediately after a watch
    // can get stuck with an empty view until another change is observed
    mostRecentTick_++;
    gettimeofday(&start, NULL);
    pending_.lock()->add(root->root_path, start, W_PENDING_RECURSIVE);
    // There is the potential for a subtle race condition here.  The boolean
    // parameter indicates whether we want to merge in the set of
    // notifications pending from the watcher or not.  Since we now coalesce
    // overlaps we must consume our outstanding set before we merge in any
    // new kernel notification information or we risk missing out on
    // observing changes that happen during the initial crawl.  This
    // translates to a two level loop; the outer loop sweeps in data from
    // inotify, then the inner loop processes it and any dirs that we pick up
    // from recursive processing.
    while (true) {
      auto [continueProcessingOuter, _] =
          processPending(root, view, pending, true);
      if (continueProcessingOuter ==
          ProcessPendingRet::ContinueProcessing::No) {
        break;
      }

      while (true) {
        auto [continueProcessingInner, _] =
            processPending(root, view, pending, false);
        if (continueProcessingInner ==
            ProcessPendingRet::ContinueProcessing::No) {
          break;
        }
      }
    }
    {
      auto lockPair = acquireLockedPair(root->recrawlInfo, crawlState_);
      lockPair.first->shouldRecrawl = false;
      lockPair.first->crawlFinish = std::chrono::steady_clock::now();
      if (lockPair.second->promise) {
        lockPair.second->promise->set_value();
        lockPair.second->promise.reset();
      }
      root->inner.done_initial = true;
    }
    root->cookies.abortAllCookies();
  }
  sample.add_root_meta(root);

  sample.finish();
  sample.force_log();
  sample.log();

  logf(
      ERR,
      "{}crawl complete\n",
      root->recrawlInfo.rlock()->recrawlCount ? "re" : "");
}

// Performs settle-time actions.
// Returns true if the root was reaped and the io thread should terminate.
static bool do_settle_things(const std::shared_ptr<watchman_root>& root) {
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

void InMemoryView::clientModeCrawl(const std::shared_ptr<watchman_root>& root) {
  PendingChanges pending;
  fullCrawl(root, pending);
}

bool InMemoryView::handleShouldRecrawl(
    const std::shared_ptr<watchman_root>& root) {
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

void InMemoryView::ioThread(const std::shared_ptr<watchman_root>& root) {
  PendingChanges localPending;

  int timeoutms = root->trigger_settle;

  // Upper bound on sleep delay.  These options are measured in seconds.
  int biggest_timeout = root->gc_interval;
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
    if (!root->inner.done_initial) {
      /* first order of business is to find all the files under our root */
      fullCrawl(root, localPending);

      timeoutms = root->trigger_settle;
    }

    // Wait for the notify thread to give us pending items, or for
    // the settle period to expire
    bool pinged;
    {
      logf(DBG, "poll_events timeout={}ms\n", timeoutms);
      auto targetPendingLock =
          pending_.lockAndWait(std::chrono::milliseconds(timeoutms), pinged);
      logf(DBG, " ... wake up (pinged={})\n", pinged);
      localPending.append(targetPendingLock->stealItems());
    }

    if (handleShouldRecrawl(root)) {
      fullCrawl(root, localPending);
      timeoutms = root->trigger_settle;
      continue;
    }

    if (!pinged && localPending.size() == 0) {
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
        localPending.clear();
        continue;
      }

      mostRecentTick_++;

      bool needAbortCookies = false;

      while (true) {
        auto [continueProcessing, isDesynced] =
            processPending(root, view, localPending, false);

        needAbortCookies |= (isDesynced == ProcessPendingRet::IsDesynced::Yes);

        if (continueProcessing == ProcessPendingRet::ContinueProcessing::No) {
          break;
        }
      }

      if (needAbortCookies) {
        logf(ERR, "recrawl complete, aborting all pending cookies\n");
        root->cookies.abortAllCookies();
      }
    }
  }
}

void InMemoryView::processPath(
    const std::shared_ptr<watchman_root>& root,
    SyncView::LockedPtr& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const watchman_dir_ent* pre_stat) {
  w_assert(
      pending.path.size() >= rootPath_.size(),
      "full_path must be a descendant of the root directory\n");
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
  if (cookies_.isCookiePrefix(pending.path)) {
    bool consider_cookie;
    if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
      // The watcher gives us file level notification, thus only consider
      // cookies if this path is coming directly from the watcher, not from a
      // recursive crawl.
      consider_cookie =
          pending.flags & W_PENDING_VIA_NOTIFY || !root->inner.done_initial;
    } else {
      // If we are de-synced, we shouldn't consider cookies as we are currently
      // walking directories recursively and we need to wait for after the
      // directory is fully re-crawled before notifying the cookie. At the end
      // of the crawl, cookies will be cancelled and re-created.
      consider_cookie =
          (pending.flags & W_PENDING_IS_DESYNCED) != W_PENDING_IS_DESYNCED;
    }

    if (consider_cookie) {
      cookies_.notifyCookie(pending.path);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(pending.path, rootPath_) ||
      (pending.flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(root, view, coll, pending);
  } else {
    statPath(root, view, coll, pending, pre_stat);
  }
}

InMemoryView::ProcessPendingRet InMemoryView::processPending(
    const std::shared_ptr<watchman_root>& root,
    SyncView::LockedPtr& view,
    PendingChanges& coll,
    bool pullFromRoot) {
  if (pullFromRoot) {
    coll.append(pending_.lock()->stealItems());
  }

  if (!coll.size()) {
    return {
        ProcessPendingRet::ContinueProcessing::No,
        ProcessPendingRet::IsDesynced::No};
  }

  logf(DBG, "processing {} events in {}\n", coll.size(), rootPath_);

  auto pending = coll.stealItems();

  auto desyncState = ProcessPendingRet::IsDesynced::No;
  while (pending) {
    if (!stopThreads_) {
      if (pending->flags & W_PENDING_IS_DESYNCED) {
        // The watcher is desynced but some cookies might be written to disk
        // while the recursive crawl is ongoing. We are going to specifically
        // ignore these cookies during that recursive crawl to avoid a race
        // condition where cookies might be seen before some files have been
        // observed as changed on disk. Due to this, and the fact that cookies
        // notifications might simply have been dropped by the watcher, we need
        // to abort the pending cookies to force them to be recreated on disk,
        // and thus re-seen.
        if (pending->flags & W_PENDING_CRAWL_ONLY) {
          desyncState = ProcessPendingRet::IsDesynced::Yes;
        }
      }

      processPath(root, view, coll, *pending, nullptr);
    }

    pending = std::move(pending->next);
  }

  return {ProcessPendingRet::ContinueProcessing::Yes, desyncState};
}
} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
