/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Returns true if we observe the change within the requested
 * time, false otherwise.
 * Must be called with the root UNLOCKED.  This function
 * will acquire and release the root lock.
 */
bool w_root_sync_to_now(struct unlocked_watchman_root *unlocked,
                        int timeoutms) {
  uint32_t tick;
  struct watchman_query_cookie cookie;
  w_string_t *path_str;
  w_stm_t file;
  int errcode = 0;
  struct timespec deadline;
  w_perf_t sample;
  struct write_locked_watchman_root lock;

  w_perf_start(&sample, "sync_to_now");

  if (pthread_cond_init(&cookie.cond, NULL)) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: cond_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }

  if (pthread_mutex_init(&cookie.lock, NULL)) {
    errcode = errno;
    pthread_cond_destroy(&cookie.cond);
    w_log(W_LOG_ERR, "sync_to_now: mutex_init failed: %s\n", strerror(errcode));
    errno = errcode;
    return false;
  }
  cookie.seen = false;
  pthread_mutex_lock(&cookie.lock);

  /* generate a cookie name: cookie prefix + id */
  w_root_lock(unlocked, "w_root_sync_to_now", &lock);
  tick = lock.root->inner.ticks++;
  path_str = w_string_make_printf(
      "%.*s%" PRIu32 "-%" PRIu32,
      lock.root->query_cookie_prefix->len,
      lock.root->query_cookie_prefix->buf,
      lock.root->inner.number,
      tick);
  /* insert our cookie in the map */
  w_ht_set(lock.root->query_cookies, w_ht_ptr_val(path_str),
      w_ht_ptr_val(&cookie));
  w_root_unlock(&lock, unlocked);

  /* touch the file */
  file = w_stm_open(path_str->buf, O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC, 0700);
  if (!file) {
    errcode = errno;
    w_log(W_LOG_ERR, "sync_to_now: creat(%s) failed: %s\n",
        path_str->buf, strerror(errcode));
    goto out;
  }
  w_stm_close(file);

  /* compute deadline */
  w_timeoutms_to_abs_timespec(timeoutms, &deadline);

  w_log(W_LOG_DBG, "sync_to_now [%s] waiting\n", path_str->buf);

  /* timed cond wait (unlocks root lock, reacquires) */
  while (!cookie.seen) {
    errcode = pthread_cond_timedwait(&cookie.cond, &cookie.lock, &deadline);
    if (errcode && !cookie.seen) {
      w_log(W_LOG_ERR,
          "sync_to_now: %s timedwait failed: %d: istimeout=%d %s\n",
          path_str->buf, errcode, errcode == ETIMEDOUT, strerror(errcode));
      goto out;
    }
  }
  w_log(W_LOG_DBG, "sync_to_now [%s] done\n", path_str->buf);

out:
  pthread_mutex_unlock(&cookie.lock);
  w_root_lock(unlocked, "w_root_sync_to_now_done", &lock);

  // can't unlink the file until after the cookie has been observed because
  // we don't know which file got changed until we look in the cookie dir
  unlink(path_str->buf);
  w_ht_del(lock.root->query_cookies, w_ht_ptr_val(path_str));
  w_root_unlock(&lock, unlocked);

  // We want to know about all timeouts
  if (!cookie.seen) {
    w_perf_force_log(&sample);
  }

  if (w_perf_finish(&sample)) {
    w_perf_add_root_meta(&sample, unlocked->root);
    w_perf_add_meta(&sample, "sync_to_now",
                    json_pack("{s:b, s:i, s:i}",      //
                              "success", cookie.seen, //
                              "timeoutms", timeoutms, //
                              "errcode", errcode      //
                              ));
    w_perf_log(&sample);
  }

  w_string_delref(path_str);
  pthread_cond_destroy(&cookie.cond);
  pthread_mutex_destroy(&cookie.lock);

  w_perf_destroy(&sample);

  if (!cookie.seen) {
    errno = errcode;
    return false;
  }

  return true;
}


/* vim:ts=2:sw=2:et:
 */
