/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
#include "watchman_error_category.h"

using namespace watchman;

void watchman_root::syncToNow(std::chrono::milliseconds timeout) {
  w_perf_t sample("sync_to_now");
  auto root = shared_from_this();
  try {
    view()->syncToNow(root, timeout);
    if (sample.finish()) {
      sample.add_root_meta(root);
      sample.add_meta(
          "sync_to_now",
          json_object({{"success", json_boolean(true)},
                       {"timeoutms", json_integer(timeout.count())}}));
      sample.log();
    }
  } catch (const std::exception& exc) {
    sample.force_log();
    sample.finish();
    sample.add_root_meta(root);
    sample.add_meta(
        "sync_to_now",
        json_object({{"success", json_boolean(false)},
                     {"reason", w_string_to_json(exc.what())},
                     {"timeoutms", json_integer(timeout.count())}}));
    sample.log();
    throw;
  }
}

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
void watchman::InMemoryView::syncToNow(
    const std::shared_ptr<w_root_t>& root,
    std::chrono::milliseconds timeout) {
  try {
    cookies_.syncToNow(timeout);
  } catch (const std::system_error& exc) {
    if (exc.code() == watchman::error_code::no_such_file_or_directory ||
        exc.code() == watchman::error_code::permission_denied ||
        exc.code() == watchman::error_code::not_a_directory) {
      // A key path was removed; this is either the vcs dir (.hg, .git, .svn)
      // or possibly the root of the watch itself.
      if (cookies_.cookieDir() == root_path) {
        // If the root was removed then we need to cancel the watch.
        // We may have already observed the removal via the notifythread,
        // but in some cases (eg: btrfs subvolume deletion) no notification
        // is received.
        root->cancel();
        throw std::runtime_error("root dir was removed or is inaccessible");
      } else {
        // The cookie dir was a VCS subdir and it got deleted.  Let's
        // focus instead on the parent dir and recursively retry.
        cookies_.setCookieDir(root_path);
        return cookies_.syncToNow(timeout);
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
      struct timeval now;
      gettimeofday(&now, nullptr);

      auto lock = pending_.lock();
      lock->add(cookies_.cookieDir(), now, W_PENDING_CRAWL_ONLY);
      lock->ping();
    }

    // We didn't have any useful additional contextual information
    // to add so let's just bubble up the exception.
    throw;
  }
}

/* vim:ts=2:sw=2:et:
 */
