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

  while (!unlocked->root->cancelled) {
    bool pinged;

    if (!unlocked->root->done_initial) {
      struct timeval start;
      w_perf_t sample;

      w_perf_start(&sample, "full-crawl");

      /* first order of business is to find all the files under our root */
      if (cfg_get_bool(unlocked->root, "iothrottle", false)) {
        w_ioprio_set_low();
      }
      w_root_lock(unlocked, "io_thread: bump ticks", &lock);
      // Ensure that we observe these files with a new, distinct clock,
      // otherwise a fresh subscription established immediately after a watch
      // can get stuck with an empty view until another change is observed
      lock.root->ticks++;
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
      lock.root->done_initial = true;
      w_perf_add_root_meta(&sample, lock.root);
      w_root_unlock(&lock, unlocked);

      if (cfg_get_bool(unlocked->root, "iothrottle", false)) {
        w_ioprio_set_normal();
      }

      w_perf_finish(&sample);
      w_perf_force_log(&sample);
      w_perf_log(&sample);
      w_perf_destroy(&sample);

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
      if (!lock.root->done_initial) {
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
    if (!lock.root->done_initial) {
      // we need to recrawl.  Discard these notifications
      w_pending_coll_drain(&pending);
      w_root_unlock(&lock, unlocked);
      continue;
    }

    lock.root->ticks++;
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

void *run_io_thread(void *arg)
{
  struct unlocked_watchman_root unlocked = {arg};

  w_set_thread_name("io %.*s", unlocked.root->root_path->len,
                    unlocked.root->root_path->buf);
  io_thread(&unlocked);
  w_log(W_LOG_DBG, "out of loop\n");

  w_root_delref(unlocked.root);
  return 0;
}

/* vim:ts=2:sw=2:et:
 */
