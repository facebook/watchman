/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/chrono.h>
#include <chrono>
#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/root/Root.h"
#include "watchman/root/warnerr.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_dir.h"
#include "watchman/watchman_file.h"

namespace watchman {

std::shared_future<void> InMemoryView::waitUntilReadyToQuery(
    const std::shared_ptr<Root>& root) {
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
    const std::shared_ptr<Root>& root,
    PendingCollection& pendingFromWatcher,
    PendingChanges& localPending) {
  root->recrawlInfo.wlock()->crawlStart = std::chrono::steady_clock::now();

  PerfSample sample("full-crawl");

  auto view = view_.wlock();
  // Ensure that we observe these files with a new, distinct clock,
  // otherwise a fresh subscription established immediately after a watch
  // can get stuck with an empty view until another change is observed
  mostRecentTick_.fetch_add(1, std::memory_order_acq_rel);

  auto start = std::chrono::system_clock::now();
  pendingFromWatcher.lock()->add(root->root_path, start, W_PENDING_RECURSIVE);
  while (true) {
    // There is the potential for a subtle race condition here.  Since we now
    // coalesce overlaps we must consume our outstanding set before we merge
    // in any new kernel notification information or we risk missing out on
    // observing changes that happen during the initial crawl.  This
    // translates to a two level loop; the outer loop sweeps in data from
    // inotify, then the inner loop processes it and any dirs that we pick up
    // from recursive processing.
    {
      auto lock = pendingFromWatcher.lock();
      localPending.append(lock->stealItems(), lock->stealSyncs());
    }
    if (localPending.empty()) {
      break;
    }

    (void)processAllPending(root, *view, localPending);
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

  root->addPerfSampleMetadata(sample);

  sample.finish();
  sample.force_log();
  sample.log();

  logf(ERR, "{}crawl complete\n", recrawlCount ? "re" : "");
}

InMemoryView::Continue InMemoryView::doSettleThings(Root& root) {
  // No new pending items were given to us, so consider that
  // we may now be settled.

  if (!root.inner.done_initial.load(std::memory_order_acquire)) {
    // we need to recrawl, stop what we're doing here
    return Continue::Continue;
  }

  warmContentCache();

  root.unilateralResponses->enqueue(json_object({{"settled", json_true()}}));

  if (root.considerReap()) {
    root.stopWatch();
    return Continue::Stop;
  }

  root.considerAgeOut();
  return Continue::Continue;
}

void InMemoryView::clientModeCrawl(const std::shared_ptr<Root>& root) {
  PendingChanges pending;
  fullCrawl(root, pendingFromWatcher_, pending);
}

bool InMemoryView::handleShouldRecrawl(Root& root) {
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

namespace {

std::chrono::milliseconds getBiggestTimeout(const Root& root) {
  std::chrono::milliseconds biggest_timeout = root.gc_interval;

  if (biggest_timeout.count() == 0 ||
      (root.idle_reap_age.count() != 0 &&
       root.idle_reap_age < biggest_timeout)) {
    biggest_timeout = root.idle_reap_age;
  }
  if (biggest_timeout.count() == 0) {
    biggest_timeout = std::chrono::hours(24);
  }
  return biggest_timeout;
}

} // namespace

void InMemoryView::ioThread(const std::shared_ptr<Root>& root) {
  IoThreadState state{getBiggestTimeout(*root)};
  state.currentTimeout = root->trigger_settle;

  while (Continue::Continue == stepIoThread(root, state, pendingFromWatcher_)) {
  }
}

InMemoryView::Continue InMemoryView::stepIoThread(
    const std::shared_ptr<Root>& root,
    IoThreadState& state,
    PendingCollection& pendingFromWatcher) {
  if (stopThreads_.load(std::memory_order_acquire)) {
    return Continue::Stop;
  }

  if (!root->inner.done_initial.load(std::memory_order_acquire)) {
    /* first order of business is to find all the files under our root */
    fullCrawl(root, pendingFromWatcher, state.localPending);

    state.currentTimeout = root->trigger_settle;
  }

  // Wait for the notify thread to give us pending items, or for
  // the settle period to expire
  bool pinged;
  {
    logf(DBG, "poll_events timeout={}ms\n", state.currentTimeout);
    auto targetPendingLock =
        pendingFromWatcher.lockAndWait(state.currentTimeout, pinged);
    logf(DBG, " ... wake up (pinged={})\n", pinged);
    state.localPending.append(
        targetPendingLock->stealItems(), targetPendingLock->stealSyncs());
  }

  // Do we need to recrawl?
  if (handleShouldRecrawl(*root)) {
    // TODO: can this just continue? handleShouldRecrawl sets done_initial to
    // false.
    fullCrawl(root, pendingFromWatcher, state.localPending);
    state.currentTimeout = root->trigger_settle;
    return Continue::Continue;
  }

  // Waiting for an event timed out, so consider the root settled.
  if (!pinged && state.localPending.empty()) {
    if (Continue::Stop == doSettleThings(*root)) {
      return Continue::Stop;
    }
    state.currentTimeout =
        std::min(state.biggestTimeout, state.currentTimeout * 2);
    return Continue::Continue;
  }

  // Otherwise we have pending items to stat and crawl

  // We are now, by definition, unsettled, so reduce sleep timeout
  // to the settle duration ready for the next loop through
  state.currentTimeout = root->trigger_settle;

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

  auto isDesynced = processAllPending(root, *view, state.localPending);
  if (isDesynced == IsDesynced::Yes) {
    logf(ERR, "recrawl complete, aborting all pending cookies\n");
    root->cookies.abortAllCookies();
  }
  return Continue::Continue;
}

InMemoryView::IsDesynced InMemoryView::processAllPending(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll) {
  auto desyncState = IsDesynced::No;

  // Don't resolve any of these until any recursive crawls are done.
  std::vector<std::vector<folly::Promise<folly::Unit>>> allSyncs;

  while (!coll.empty()) {
    logf(
        DBG,
        "processing {} events in {}\n",
        coll.getPendingItemCount(),
        rootPath_);

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
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const DirEntry* pre_stat) {
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
  if (root->cookies.isCookiePrefix(pending.path)) {
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
      root->cookies.notifyCookie(pending.path);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(pending.path, rootPath_) ||
      (pending.flags & W_PENDING_CRAWL_ONLY)) {
    crawler(root, view, coll, pending);
  } else {
    statPath(*root, root->cookies, view, coll, pending, pre_stat);
  }
}

namespace {

void apply_dir_size_hint(watchman_dir* dir, uint32_t ndirs, uint32_t nfiles) {
  if (dir->files.empty() && nfiles > 0) {
    dir->files.reserve(nfiles);
  }
  if (dir->dirs.empty() && ndirs > 0) {
    dir->dirs.reserve(ndirs);
  }
}

} // namespace

void InMemoryView::crawler(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending) {
  bool recursive = pending.flags.contains(W_PENDING_RECURSIVE);

  bool stat_all;
  if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
    stat_all = watcher_->flags & WATCHER_COALESCED_RENAME;
  } else {
    // If the watcher doesn't give us per-file notifications for watched dirs
    // and is able to watch files individually, then we'll end up explicitly
    // tracking them and will get updates for the files explicitly. We don't
    // need to look at the files again when we crawl. To avoid recursing into
    // all the subdirectories, only stat all the files/directories when this
    // directory was added by the watcher.
    stat_all = pending.flags.contains(W_PENDING_NONRECURSIVE_SCAN);
  }

  auto dir = view.resolveDir(pending.path, true);

  // Detect root directory replacement.
  // The inode number check is handled more generally by the sister code
  // in stat.cpp.  We need to special case it for the root because we never
  // generate a watchman_file node for the root and thus never call
  // InMemoryView::statPath (we'll fault if we do!).
  // Ideally the kernel would have given us a signal when we've been replaced
  // but some filesystems (eg: BTRFS) do not emit appropriate inotify events
  // for things like subvolume deletes.  We've seen situations where the
  // root has been replaced and we got no notifications at all and this has
  // left the cookie sync mechanism broken forever.
  if (pending.path == root->root_path) {
    try {
      auto st = fileSystem_.getFileInformation(
          pending.path.c_str(), root->case_sensitive);
      if (st.ino != view.getRootInode()) {
        // If it still exists and the inode doesn't match, then we need
        // to force recrawl to make sure we're in sync.
        // We're lazily initializing the rootInode to 0 here, so we don't
        // need to do this the first time through (we're already crawling
        // everything in that case).
        if (view.getRootInode() != 0) {
          root->scheduleRecrawl(
              "root was replaced and we didn't get notified by the kernel");
          return;
        }
        recursive = true;
        view.setRootInode(st.ino);
      }
    } catch (const std::system_error& err) {
      handle_open_errno(
          *root, dir, pending.now, "getFileInformation", err.code());
      view.markDirDeleted(*watcher_, dir, getClock(pending.now), true);
      return;
    }
  }

  char path[WATCHMAN_NAME_MAX];
  memcpy(path, pending.path.data(), pending.path.size());
  path[pending.path.size()] = 0;

  logf(
      DBG, "opendir({}) recursive={} stat_all={}\n", path, recursive, stat_all);

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  std::unique_ptr<DirHandle> osdir;

  try {
    osdir = watcher_->startWatchDir(root, dir, path);
  } catch (const std::system_error& err) {
    logf(DBG, "startWatchDir({}) threw {}\n", path, err.what());
    handle_open_errno(*root, dir, pending.now, "opendir", err.code());
    view.markDirDeleted(*watcher_, dir, getClock(pending.now), true);
    return;
  }

  if (dir->files.empty()) {
    // Pre-size our hash(es) if we can, so that we can avoid collisions
    // and re-hashing during initial crawl
    uint32_t num_dirs = 0;
#ifndef _WIN32
    struct stat st;
    int dfd = osdir->getFd();
    if (dfd != -1 && fstat(dfd, &st) == 0) {
      num_dirs = (uint32_t)st.st_nlink;
    }
#endif
    // st.st_nlink is usually number of dirs + 2 (., ..).
    // If it is less than 2 then it doesn't follow that convention.
    // We just pass it through for the dir size hint and the hash
    // table implementation will round that up to the next power of 2
    apply_dir_size_hint(
        dir,
        num_dirs,
        uint32_t(root->config.getInt("hint_num_files_per_dir", 64)));
  }

  /* flag for delete detection */
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists) {
      file->maybe_deleted = true;
    }
  }

  try {
    while (const DirEntry* dirent = osdir->readDir()) {
      // Don't follow parent/self links
      if (dirent->d_name[0] == '.' &&
          (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))) {
        continue;
      }

      // Queue it up for analysis if the file is newly existing
      w_string name(dirent->d_name, W_STRING_BYTE);
      struct watchman_file* file = dir->getChildFile(name);
      if (file) {
        file->maybe_deleted = false;
      }
      if (!file || !file->exists || stat_all || recursive) {
        auto full_path = dir->getFullPathToChild(name);

        PendingFlags newFlags;
        if (recursive || !file || !file->exists) {
          newFlags.set(W_PENDING_RECURSIVE);
        }
        if (pending.flags & W_PENDING_IS_DESYNCED) {
          newFlags.set(W_PENDING_IS_DESYNCED);
        }

        logf(
            DBG,
            "in crawler calling processPath on {} oldflags={} newflags={}\n",
            full_path,
            pending.flags.asRaw(),
            newFlags.asRaw());

        processPath(
            root,
            view,
            coll,
            PendingChange{std::move(full_path), pending.now, newFlags},
            dirent);
      }
    }
  } catch (const std::system_error& exc) {
    log(ERR,
        "Error while reading dir ",
        path,
        ": ",
        exc.what(),
        ", re-adding to pending list to re-assess\n");
    coll.add(path, pending.now, {});
  }
  osdir.reset();

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists &&
        (file->maybe_deleted || (file->stat.isDir() && recursive))) {
      coll.add(
          dir,
          file->getName().data(),
          pending.now,
          recursive ? W_PENDING_RECURSIVE : PendingFlags{});
    }
  }
}

namespace {
bool did_file_change(
    const watchman::FileInformation* saved,
    const watchman::FileInformation* fresh) {
  /* we have to compare this way because the stat structure
   * may contain fields that vary and that don't impact our
   * understanding of the file */

#define FIELD_CHG(name)             \
  if (saved->name != fresh->name) { \
    return true;                    \
  }

  // Can't compare with memcmp due to padding and garbage in the struct
  // on OpenBSD, which has a 32-bit tv_sec + 64-bit tv_nsec
#define TIMESPEC_FIELD_CHG(wat)                           \
  {                                                       \
    struct timespec a = saved->wat##time;                 \
    struct timespec b = fresh->wat##time;                 \
    if (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec) { \
      return true;                                        \
    }                                                     \
  }

  FIELD_CHG(mode);

  if (!saved->isDir()) {
    FIELD_CHG(size);
    FIELD_CHG(nlink);
  }
  FIELD_CHG(dev);
  FIELD_CHG(ino);
  FIELD_CHG(uid);
  FIELD_CHG(gid);
  // Don't care about st_blocks
  // Don't care about st_blksize
  // Don't care about st_atimespec
  TIMESPEC_FIELD_CHG(m);
  TIMESPEC_FIELD_CHG(c);

  return false;
}
} // namespace

/**
 * The purpose of this function is to help us decide whether we should
 * update the parent directory when a non-directory directory entry is changed.
 * If so, we schedule re-examining the parent. Not all systems report the
 * containing directory as changed in that situation, so we decide this based on
 * the capabilities of the watcher. If the directory is added to the
 * PendingCollection, this function returns true. Otherwise, this function
 * returns false.
 */
bool InMemoryView::propagateToParentDirIfAppropriate(
    const RootConfig& root,
    PendingChanges& coll,
    std::chrono::system_clock::time_point now,
    const FileInformation& entryStat,
    const w_string& dirName,
    const watchman_dir* parentDir,
    bool isUnlink) {
  if ((watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) &&
      dirName != root.root_path && !entryStat.isDir() &&
      parentDir->last_check_existed) {
    /* We're deliberately not propagating any of the flags through
     * from statPath() (which calls us); we
     * definitely don't want this to be a recursive evaluation.
     * Previously, we took pains to avoid turning on VIA_NOTIFY
     * here to avoid spuriously marking the node as changed when
     * only its atime was changed to avoid tickling some behavior
     * in the Pants build system:
     * https://github.com/facebook/watchman/issues/305 and
     * https://github.com/facebook/watchman/issues/307, but
     * unfortunately we do need to set it here because eg:
     * Linux doesn't send an inotify event for the parent
     * directory for an unlink, and if we rely on stat()
     * alone, the filesystem mtime granularity may be too
     * low for us to detect that the parent has changed.
     * As a compromize, if we're told that the change was due
     * to an unlink, then we force delivery of a change event,
     * otherwise we'll only do so if the directory has
     * observably changed via stat().
     */
    coll.add(dirName, now, isUnlink ? W_PENDING_VIA_NOTIFY : PendingFlags{});
    return true;
  }
  return false;
}

void InMemoryView::statPath(
    const RootConfig& root,
    const CookieSync& cookies,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const DirEntry* pre_stat) {
  bool recursive = pending.flags.contains(W_PENDING_RECURSIVE);
  const bool via_notify = pending.flags.contains(W_PENDING_VIA_NOTIFY);
  const PendingFlags desynced_flag = pending.flags & W_PENDING_IS_DESYNCED;

  if (root.ignore.isIgnoreDir(pending.path)) {
    logf(DBG, "{} matches ignore_dir rules\n", pending.path);
    return;
  }

  char path[WATCHMAN_NAME_MAX];
  if (pending.path.size() > sizeof(path) - 1) {
    logf(FATAL, "path {} is too big\n", pending.path);
  }

  memcpy(path, pending.path.data(), pending.path.size());
  path[pending.path.size()] = 0;

  auto dir_name = pending.path.dirName();
  auto file_name = pending.path.baseName();
  auto parentDir = view.resolveDir(dir_name, true);

  auto file = parentDir->getChildFile(file_name);

  auto dir_ent = parentDir->getChildDir(file_name);

  watchman::FileInformation st;
  std::error_code errcode;
  if (pre_stat && pre_stat->has_stat) {
    st = pre_stat->stat;
  } else {
    try {
      st = fileSystem_.getFileInformation(path, root.case_sensitive);
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          "\n");
    } catch (const std::system_error& exc) {
      errcode = exc.code();
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          " failed: ",
          exc.what(),
          "\n");
    }
  }

  if (processedPaths_) {
    processedPaths_->write(PendingChangeLogEntry{pending, errcode, st});
  }

  if (errcode == watchman::error_code::no_such_file_or_directory ||
      errcode == watchman::error_code::not_a_directory) {
    /* it's not there, update our state */
    if (dir_ent) {
      view.markDirDeleted(*watcher_, dir_ent, getClock(pending.now), true);
      watchman::log(
          watchman::DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " so stopping watch\n");
    }
    if (file) {
      if (file->exists) {
        watchman::log(
            watchman::DBG,
            "getFileInformation(",
            path,
            ") -> ",
            errcode.message(),
            " so marking ",
            file->getName(),
            " deleted\n");
        file->exists = false;
        view.markFileChanged(*watcher_, file, getClock(pending.now));
      }
    } else {
      // It was created and removed before we could ever observe it
      // in the filesystem.  We need to generate a deleted file
      // representation of it now, so that subscription clients can
      // be notified of this event
      file = view.getOrCreateChildFile(
          *watcher_, parentDir, file_name, getClock(pending.now));
      log(DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " and file node was NULL. "
          "Generating a deleted node.\n");
      file->exists = false;
      view.markFileChanged(*watcher_, file, getClock(pending.now));
    }

    if (!propagateToParentDirIfAppropriate(
            root,
            coll,
            pending.now,
            file->stat,
            dir_name,
            parentDir,
            /* isUnlink= */ true) &&
        root.case_sensitive == CaseSensitivity::CaseInSensitive &&
        !w_string_equal(dir_name, root.root_path) &&
        parentDir->last_check_existed) {
      /* If we rejected the name because it wasn't canonical,
       * we need to ensure that we look in the parent dir to discover
       * the new item(s) */
      logf(
          DBG,
          "we're case insensitive, and {} is ENOENT, "
          "speculatively look at parent dir {}\n",
          path,
          dir_name);
      coll.add(dir_name, pending.now, W_PENDING_CRAWL_ONLY);
    }

  } else if (errcode.value()) {
    log(ERR,
        "getFileInformation(",
        path,
        ") failed and not handled! -> ",
        errcode.message(),
        " value=",
        errcode.value(),
        " category=",
        errcode.category().name(),
        "\n");
  } else {
    if (!file) {
      file = view.getOrCreateChildFile(
          *watcher_, parentDir, file_name, getClock(pending.now));
    }

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = mostRecentTick_;
      file->ctime.timestamp = std::chrono::system_clock::to_time_t(pending.now);
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->stat, &st)) {
      logf(
          DBG,
          "file changed exists={} via_notify={} stat-changed={} isdir={} size={} {}\n",
          file->exists,
          via_notify,
          file->exists && !via_notify,
          st.isDir(),
          st.size,
          path);
      file->exists = true;
      view.markFileChanged(*watcher_, file, getClock(pending.now));

      // If the inode number changed then we definitely need to recursively
      // examine any children because we cannot assume that the kernel will
      // have given us the correct hints about this change.  BTRFS is one
      // example of a filesystem where this has been observed to happen.
      if (file->stat.ino != st.ino) {
        recursive = true;
      }
    }

    memcpy(&file->stat, &st, sizeof(file->stat));

    if (st.isDir()) {
      if (dir_ent == NULL) {
        recursive = true;
      } else {
        // Ensure that we believe that this node exists
        dir_ent->last_check_existed = true;
      }

      // Don't recurse if our parent is an ignore dir
      if (!root.ignore.isIgnoreVCS(dir_name) ||
          // but do if we're looking at the cookie dir (stat_path is never
          // called for the root itself)
          cookies.isCookieDir(pending.path)) {
        if (recursive) {
          /* we always need to crawl if we're recursive, this can happen when a
           * directory is created */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY);
        } else if (pending.flags & W_PENDING_NONRECURSIVE_SCAN) {
          /* on file changes, we receive a notification on the directory and
           * thus we just need to crawl this one directory to consider all
           * the pending files. */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_NONRECURSIVE_SCAN |
                  W_PENDING_CRAWL_ONLY);
        } else {
          if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
            /* we get told about changes on the child, so we don't need to do
             * anything */
          } else {
            /* in all the other cases, crawl */
            coll.add(
                pending.path,
                pending.now,
                desynced_flag | W_PENDING_CRAWL_ONLY);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      view.markDirDeleted(*watcher_, dir_ent, getClock(pending.now), true);
    }
    propagateToParentDirIfAppropriate(
        root,
        coll,
        pending.now,
        st,
        dir_name,
        parentDir,
        /* isUnlink= */ false);
  }
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
