/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void io_thread(struct unlocked_watchman_root *unlocked)
{
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

  w_pending_coll_init(&pending);

  while (!unlocked->root->inner.cancelled) {
    bool pinged;

    if (!unlocked->root->inner.done_initial) {
      struct timeval start;
      w_perf_t sample("full-crawl");

      /* first order of business is to find all the files under our root */
      if (cfg_get_bool(unlocked->root, "iothrottle", false)) {
        w_ioprio_set_low();
      }
      w_root_lock(unlocked, "io_thread: bump ticks", &lock);
      // Ensure that we observe these files with a new, distinct clock,
      // otherwise a fresh subscription established immediately after a watch
      // can get stuck with an empty view until another change is observed
      lock.root->inner.ticks++;
      gettimeofday(&start, NULL);
      w_pending_coll_add(&lock.root->pending, lock.root->root_path, start, 0);
      // There is the potential for a subtle race condition here.  The boolean
      // parameter indicates whether we want to merge in the set of
      // notifications pending from the watcher or not.  Since we now coalesce
      // overlaps we must consume our outstanding set before we merge in any
      // new kernel notification information or we risk missing out on
      // observing changes that happen during the initial crawl.  This
      // translates to a two level loop; the outer loop sweeps in data from
      // inotify, then the inner loop processes it and any dirs that we pick up
      // from recursive processing.
      while (w_root_process_pending(&lock, &pending, true)) {
        while (w_root_process_pending(&lock, &pending, false)) {
          ;
        }
      }
      lock.root->inner.done_initial = true;
      sample.add_root_meta(lock.root);
      w_root_unlock(&lock, unlocked);

      if (cfg_get_bool(unlocked->root, "iothrottle", false)) {
        w_ioprio_set_normal();
      }

      sample.finish();
      sample.force_log();
      sample.log();

      w_log(W_LOG_ERR, "%scrawl complete\n",
            unlocked->root->recrawl_count ? "re" : "");
      timeoutms = unlocked->root->trigger_settle;
    }

    // Wait for the notify thread to give us pending items, or for
    // the settle period to expire
    w_log(W_LOG_DBG, "poll_events timeout=%dms\n", timeoutms);
    pinged = w_pending_coll_lock_and_wait(&unlocked->root->pending, timeoutms);
    w_log(W_LOG_DBG, " ... wake up (pinged=%s)\n", pinged ? "true" : "false");
    w_pending_coll_append(&pending, &unlocked->root->pending);
    w_pending_coll_unlock(&unlocked->root->pending);

    if (!pinged && w_pending_coll_size(&pending) == 0) {
      process_pending_symlink_targets(unlocked);

      // No new pending items were given to us, so consider that
      // we may now be settled.

      w_root_lock(unlocked, "io_thread: settle out", &lock);
      if (!lock.root->inner.done_initial) {
        // we need to recrawl, stop what we're doing here
        w_root_unlock(&lock, unlocked);
        continue;
      }

      process_subscriptions(&lock);
      process_triggers(&lock);
      if (consider_reap(&lock)) {
        w_root_unlock(&lock, unlocked);
        w_root_stop_watch(unlocked);
        break;
      }
      consider_age_out(&lock);
      w_root_unlock(&lock, unlocked);

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
    consider_age_out(&lock);

    while (w_root_process_pending(&lock, &pending, false)) {
      ;
    }

    w_root_unlock(&lock, unlocked);
  }

  w_pending_coll_destroy(&pending);
}

void w_root_process_path(struct write_locked_watchman_root *lock,
                         struct watchman_pending_collection *coll,
                         w_string_t *full_path, struct timeval now, int flags,
                         struct watchman_dir_ent *pre_stat) {
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
  if (w_string_startswith(full_path, lock->root->query_cookie_prefix)) {
    bool consider_cookie =
        (lock->root->watcher_ops->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS)
        ? ((flags & W_PENDING_VIA_NOTIFY) || !lock->root->inner.done_initial)
        : true;

    if (!consider_cookie) {
      // Never allow cookie files to show up in the tree
      return;
    }

    auto cookie_iter = lock->root->query_cookies.find(full_path);
    w_log(
        W_LOG_DBG,
        "cookie for %s? %s\n",
        full_path->buf,
        cookie_iter != lock->root->query_cookies.end() ? "yes" : "no");

    if (cookie_iter != lock->root->query_cookies.end()) {
      auto cookie = cookie_iter->second;
      cookie->seen = true;
      pthread_cond_signal(&cookie->cond);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (w_string_equal(full_path, lock->root->root_path)
      || (flags & W_PENDING_CRAWL_ONLY) == W_PENDING_CRAWL_ONLY) {
    crawler(lock, coll, full_path, now,
        (flags & W_PENDING_RECURSIVE) == W_PENDING_RECURSIVE);
  } else {
    stat_path(lock, coll, full_path, now, flags, pre_stat);
  }
}

bool w_root_process_pending(struct write_locked_watchman_root *lock,
    struct watchman_pending_collection *coll,
    bool pull_from_root)
{
  struct watchman_pending_fs *p, *pending;

  if (pull_from_root) {
    // You MUST own root->pending lock for this
    w_pending_coll_append(coll, &lock->root->pending);
  }

  if (!coll->pending) {
    return false;
  }

  w_log(W_LOG_DBG, "processing %d events in %s\n",
      w_pending_coll_size(coll), lock->root->root_path->buf);

  // Steal the contents
  pending = coll->pending;
  coll->pending = NULL;
  w_pending_coll_drain(coll);

  while (pending) {
    p = pending;
    pending = p->next;

    if (!lock->root->inner.cancelled) {
      w_root_process_path(lock, coll, p->path, p->now, p->flags, NULL);
    }

    w_pending_fs_free(p);
  }

  return true;
}

void *run_io_thread(void *arg)
{
  struct unlocked_watchman_root unlocked = {(w_root_t*)arg};

  w_set_thread_name("io %.*s", unlocked.root->root_path->len,
                    unlocked.root->root_path->buf);
  io_thread(&unlocked);
  w_log(W_LOG_DBG, "out of loop\n");

  w_root_delref(&unlocked);
  return 0;
}

/* vim:ts=2:sw=2:et:
 */
