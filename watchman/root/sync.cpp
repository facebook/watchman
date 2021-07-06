/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman.h"

using folly::to;
using watchman::w_perf_t;

void watchman_root::syncToNow(std::chrono::milliseconds timeout) {
  w_perf_t sample("sync_to_now");
  auto root = shared_from_this();
  try {
    view()->syncToNow(root, timeout);
    if (sample.finish()) {
      sample.add_root_meta(root);
      sample.add_meta(
          "sync_to_now",
          json_object(
              {{"success", json_boolean(true)},
               {"timeoutms", json_integer(timeout.count())}}));
      sample.log();
    }
  } catch (const std::exception& exc) {
    sample.force_log();
    sample.finish();
    sample.add_root_meta(root);
    sample.add_meta(
        "sync_to_now",
        json_object(
            {{"success", json_boolean(false)},
             {"reason", w_string_to_json(exc.what())},
             {"timeoutms", json_integer(timeout.count())}}));
    sample.log();
    throw;
  }
}

namespace watchman {

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Throws a std::system_error with an ETIMEDOUT error if
 * the timeout expires before we observe the change, or
 * a runtime_error if the root has been deleted or rendered
 * inaccessible. */
void InMemoryView::syncToNow(
    const std::shared_ptr<watchman_root>& root,
    std::chrono::milliseconds timeout) {
  syncToNowCookies(root, timeout);

  // Some watcher implementations (notably, FSEvents) reorder change events
  // before they're reported, and cookie files are not sufficient. Instead, the
  // watcher supports direct synchronization. Once a cookie file has been
  // observed, ensure that all pending events have been flushed and wait until
  // the pending event queue is fully crawled.
  auto result = watcher_->flushPendingEvents();
  if (result.valid()) {
    // The watcher has made all pending events available and inserted a promise
    // into its PendingCollection. Wait for InMemoryView to observe it and
    // everything prior.
    //
    // Would be nice to use a deadline rather than a timeout here.
    std::move(result).get(timeout);
  }
}

void InMemoryView::syncToNowCookies(
    const std::shared_ptr<watchman_root>& root,
    std::chrono::milliseconds timeout) {
  try {
    cookies_.syncToNow(timeout);
  } catch (const std::system_error& exc) {
    auto cookieDirs = cookies_.cookieDirs();

    if (exc.code() == watchman::error_code::no_such_file_or_directory ||
        exc.code() == watchman::error_code::permission_denied ||
        exc.code() == watchman::error_code::not_a_directory) {
      // A key path was removed; this is either the vcs dir (.hg, .git, .svn)
      // or possibly the root of the watch itself.
      if (!(watcher_->flags & WATCHER_HAS_SPLIT_WATCH)) {
        w_assert(
            cookieDirs.size() == 1,
            "Non split watchers cannot have multiple cookie directories");
        if (cookieDirs.count(rootPath_) == 1) {
          // If the root was removed then we need to cancel the watch.
          // We may have already observed the removal via the notifythread,
          // but in some cases (eg: btrfs subvolume deletion) no notification
          // is received.
          root->cancel();
          throw std::runtime_error("root dir was removed or is inaccessible");
        } else {
          // The cookie dir was a VCS subdir and it got deleted.  Let's
          // focus instead on the parent dir and recursively retry.
          cookies_.setCookieDir(rootPath_);
          return cookies_.syncToNow(timeout);
        }
      } else {
        // Split watchers have one watch on the root and watches for nested
        // directories, and syncToNow will only throw if no cookies were
        // created, ie: if all the nested watched directories are no longer
        // present and the root directory has been removed.
        root->cancel();
        throw std::runtime_error("root dir was removed or is inaccessible");
      }
    }

    // Let's augment the error reason with the current recrawl state,
    // if any.
    {
      auto info = root->recrawlInfo.rlock();

      if (!root->inner.done_initial || info->shouldRecrawl) {
        std::string extra = (info->recrawlCount > 0)
            ? to<std::string>("(re-crawling, count=", info->recrawlCount, ")")
            : "(performing initial crawl)";

        throw std::system_error(
            exc.code(), to<std::string>(exc.what(), ". ", extra));
      }
    }

    // On BTRFS we're not guaranteed to get notified about all classes
    // of replacement so we make a best effort attempt to do something
    // reasonable.   Let's pretend that we got notified about the cookie
    // dir changing and schedule the IO thread to look at it.
    // If it observes a change it will do the right thing.
    {
      auto now = std::chrono::system_clock::now();

      auto lock = pending_.lock();
      for (const auto& dir : cookieDirs) {
        lock->add(dir, now, W_PENDING_CRAWL_ONLY);
      }
      lock->ping();
    }

    // We didn't have any useful additional contextual information
    // to add so let's just bubble up the exception.
    throw;
  }
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
