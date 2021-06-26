/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <chrono>
#include "watchman/InMemoryView.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman.h"

namespace watchman {

std::shared_future<void> InMemoryView::waitUntilReadyToQuery(
    const std::shared_ptr<watchman_root>& root) {
  auto lockPair = acquireLockedPair(root->recrawlInfo, crawlState_);

  if (lockPair.second->promise && lockPair.second->future.valid()) {
    return lockPair.second->future;
  }

  if (root->inner.done_initial.load(std::memory_order_acquire) &&
      !lockPair.first->shouldRecrawl) {
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
  root->recrawlInfo.wlock()->crawlStart = std::chrono::steady_clock::now();

  w_perf_t sample("full-crawl");

  auto view = view_.wlock();
  // Ensure that we observe these files with a new, distinct clock,
  // otherwise a fresh subscription established immediately after a watch
  // can get stuck with an empty view until another change is observed
  mostRecentTick_.fetch_add(1, std::memory_order_acq_rel);

  auto start = std::chrono::system_clock::now();
  pending_.lock()->add(root->root_path, start, W_PENDING_RECURSIVE);
  while (true) {
    // There is the potential for a subtle race condition here.  Since we now
    // coalesce overlaps we must consume our outstanding set before we merge
    // in any new kernel notification information or we risk missing out on
    // observing changes that happen during the initial crawl.  This
    // translates to a two level loop; the outer loop sweeps in data from
    // inotify, then the inner loop processes it and any dirs that we pick up
    // from recursive processing.
    {
      auto lock = pending_.lock();
      pending.append(lock->stealItems(), lock->stealSyncs());
    }
    if (pending.empty()) {
      break;
    }

    (void)processAllPending(root, *view, pending);
  }

  auto [recrawlInfo, crawlState] =
      acquireLockedPair(root->recrawlInfo, crawlState_);
  recrawlInfo->shouldRecrawl = false;
  recrawlInfo->crawlFinish = std::chrono::steady_clock::now();
  if (crawlState->promise) {
    crawlState->promise->set_value();
    crawlState->promise.reset();
  }
  root->inner.done_initial.store(true, std::memory_order_release);

  // There is no need to hold locks while logging, and abortAllCookies resolves
  // a Promise which can run arbitrary code, so locks must be released here.
  auto recrawlCount = recrawlInfo->recrawlCount;
  recrawlInfo.unlock();
  crawlState.unlock();
  view.unlock();

  root->cookies.abortAllCookies();

  sample.add_root_meta(root);

  sample.finish();
  sample.force_log();
  sample.log();

  logf(ERR, "{}crawl complete\n", recrawlCount ? "re" : "");
}

// Performs settle-time actions.
// Returns true if the root was reaped and the io thread should terminate.
static bool do_settle_things(InMemoryView& view, watchman_root& root) {
  // No new pending items were given to us, so consider that
  // we may now be settled.

  root.processPendingSymlinkTargets();

  if (!root.inner.done_initial.load(std::memory_order_acquire)) {
    // we need to recrawl, stop what we're doing here
    return false;
  }

  view.warmContentCache();

  root.unilateralResponses->enqueue(json_object({{"settled", json_true()}}));

  if (root.considerReap()) {
    root.stopWatch();
    return true;
  }

  root.considerAgeOut();
  return false;
}

void InMemoryView::clientModeCrawl(const std::shared_ptr<watchman_root>& root) {
  PendingChanges pending;
  fullCrawl(root, pending);
}

bool InMemoryView::handleShouldRecrawl(watchman_root& root) {
  {
    auto info = root.recrawlInfo.rlock();
    if (!info->shouldRecrawl) {
      return false;
    }
  }

  if (!root.inner.cancelled) {
    auto info = root.recrawlInfo.wlock();
    info->recrawlCount++;
    root.inner.done_initial.store(false, std::memory_order_release);
  }

  return true;
}

void InMemoryView::ioThread(const std::shared_ptr<watchman_root>& root) {
  PendingChanges localPending;

  int timeoutms = root->trigger_settle;

  // Upper bound on sleep delay.  These options are measured in seconds.
  int biggest_timeout = root->gc_interval.count();
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
    if (!root->inner.done_initial.load(std::memory_order_acquire)) {
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
      localPending.append(
          targetPendingLock->stealItems(), targetPendingLock->stealSyncs());
    }

    // Do we need to recrawl?
    if (handleShouldRecrawl(*root)) {
      // TODO: can this just continue? handleShouldRecrawl sets done_initial to
      // false.
      fullCrawl(root, localPending);
      timeoutms = root->trigger_settle;
      continue;
    }

    // Waiting for an event timed out, so consider the root settled.
    if (!pinged && localPending.empty()) {
      if (do_settle_things(*this, *root)) {
        break;
      }
      timeoutms = std::min(biggest_timeout, timeoutms * 2);
      continue;
    }

    // Otherwise we have pending items to stat and crawl

    // We are now, by definition, unsettled, so reduce sleep timeout
    // to the settle duration ready for the next loop through
    timeoutms = root->trigger_settle;

    // Some Linux 5.6 kernels will report inotify events before the file has
    // been evicted from the cache, causing Watchman to incorrectly think the
    // file is still on disk after it's unlinked. If configured, allow a brief
    // sleep to mitigate.
    //
    // Careful with this knob: it adds latency to every query by delaying cookie
    // processing.
    auto notify_sleep_ms = config_.getInt("notify_sleep_ms", 0);
    if (notify_sleep_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(notify_sleep_ms));
    }

    auto view = view_.wlock();

    // fullCrawl unconditionally sets done_initial to true and if
    // handleShouldRecrawl set it false, execution wouldn't reach this part of
    // the loop.
    w_check(
        root->inner.done_initial.load(std::memory_order_acquire),
        "A full crawl should not be pending at this point in the loop.");

    mostRecentTick_.fetch_add(1, std::memory_order_acq_rel);

    auto isDesynced = processAllPending(root, *view, localPending);
    if (isDesynced == IsDesynced::Yes) {
      logf(ERR, "recrawl complete, aborting all pending cookies\n");
      root->cookies.abortAllCookies();
    }
  }
}

InMemoryView::IsDesynced InMemoryView::processAllPending(
    const std::shared_ptr<watchman_root>& root,
    ViewDatabase& view,
    PendingChanges& coll) {
  auto desyncState = IsDesynced::No;

  // Don't resolve any of these until any recursive crawls are done.
  std::vector<std::vector<folly::Promise<folly::Unit>>> allSyncs;

  while (!coll.empty()) {
    logf(DBG, "processing {} events in {}\n", coll.size(), rootPath_);

    auto pending = coll.stealItems();
    auto syncs = coll.stealSyncs();
    if (syncs.empty()) {
      w_check(
          pending != nullptr,
          "coll.stealItems() and coll.size() did not agree about its size");
    } else {
      allSyncs.push_back(std::move(syncs));
    }

    while (pending) {
      if (!stopThreads_) {
        if (pending->flags & W_PENDING_IS_DESYNCED) {
          // The watcher is desynced but some cookies might be written to disk
          // while the recursive crawl is ongoing. We are going to specifically
          // ignore these cookies during that recursive crawl to avoid a race
          // condition where cookies might be seen before some files have been
          // observed as changed on disk. Due to this, and the fact that cookies
          // notifications might simply have been dropped by the watcher, we
          // need to abort the pending cookies to force them to be recreated on
          // disk, and thus re-seen.
          if (pending->flags & W_PENDING_CRAWL_ONLY) {
            desyncState = IsDesynced::Yes;
          }
        }

        // processPath may insert new pending items into `coll`,
        processPath(root, view, coll, *pending, nullptr);
      }

      // TODO: Document that continuing to run this loop when stopThreads_ is
      // true fixes a stack overflow when pending is long.
      pending = std::move(pending->next);
    }
  }

  for (auto& outer : allSyncs) {
    for (auto& sync : outer) {
      sync.setValue();
    }
  }

  return desyncState;
}

void InMemoryView::processPath(
    const std::shared_ptr<watchman_root>& root,
    ViewDatabase& view,
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
      consider_cookie = (pending.flags & W_PENDING_VIA_NOTIFY) ||
          !root->inner.done_initial.load(std::memory_order_acquire);
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
      (pending.flags & W_PENDING_CRAWL_ONLY)) {
    crawler(root, view, coll, pending);
  } else {
    statPath(*root, view, coll, pending, pre_stat);
  }
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
