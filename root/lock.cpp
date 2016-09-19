/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#define define_lock_funcs(                                                   \
    lock_type,                                                               \
    locker,                                                                  \
    do_lock,                                                                 \
    timedlocker,                                                             \
    do_timed_lock,                                                           \
    do_try_lock,                                                             \
    unlocker)                                                                \
  void locker(                                                               \
      struct unlocked_watchman_root* unlocked,                               \
      const char* purpose,                                                   \
      lock_type* lock) {                                                     \
    int err;                                                                 \
    if (!unlocked || !unlocked->root) {                                      \
      w_log(                                                                 \
          W_LOG_FATAL,                                                       \
          "vacated or already locked root passed to " #locker                \
          "with "                                                            \
          "purpose "                                                         \
          "%s\n",                                                            \
          purpose);                                                          \
    }                                                                        \
    err = do_lock(&unlocked->root->lock);                                    \
    if (err != 0) {                                                          \
      w_log(                                                                 \
          W_LOG_FATAL,                                                       \
          "lock (%s) [%s]: %s\n",                                            \
          purpose,                                                           \
          unlocked->root->root_path.c_str(),                                 \
          strerror(err));                                                    \
    }                                                                        \
    unlocked->root->lock_reason = purpose;                                   \
    /* We've logically moved the callers root into the lock holder */        \
    lock->root = unlocked->root;                                             \
    unlocked->root = nullptr;                                                \
  }                                                                          \
  bool timedlocker(                                                          \
      struct unlocked_watchman_root* unlocked,                               \
      const char* purpose,                                                   \
      int timeoutms,                                                         \
      lock_type* lock) {                                                     \
    struct timespec ts;                                                      \
    struct timeval delta, now, target;                                       \
    int err;                                                                 \
    if (!unlocked || !unlocked->root) {                                      \
      w_log(                                                                 \
          W_LOG_FATAL,                                                       \
          "vacated or already locked root passed "                           \
          "to " #timedlocker "with purpose %s\n",                            \
          purpose);                                                          \
    }                                                                        \
    if (timeoutms <= 0) {                                                    \
      /* Special case an immediate check, because the implementation of */   \
      /* pthread_mutex_timedlock may return immediately if we are already */ \
      /* past-due. */                                                        \
      err = do_try_lock(&unlocked->root->lock);                              \
    } else {                                                                 \
      /* Add timeout to current time, convert to absolute timespec */        \
      gettimeofday(&now, nullptr);                                           \
      delta.tv_sec = timeoutms / 1000;                                       \
      delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;            \
      w_timeval_add(now, delta, &target);                                    \
      w_timeval_to_timespec(target, &ts);                                    \
      err = do_timed_lock(&unlocked->root->lock, &ts);                       \
    }                                                                        \
    if (err == ETIMEDOUT || err == EBUSY) {                                  \
      w_log(                                                                 \
          W_LOG_ERR,                                                         \
          "lock (%s) [%s] failed after %dms, current lock purpose: %s\n",    \
          purpose,                                                           \
          unlocked->root->root_path.c_str(),                                 \
          timeoutms,                                                         \
          unlocked->root->lock_reason);                                      \
      errno = ETIMEDOUT;                                                     \
      return false;                                                          \
    }                                                                        \
    if (err != 0) {                                                          \
      w_log(                                                                 \
          W_LOG_FATAL,                                                       \
          "lock (%s) [%s]: %s\n",                                            \
          purpose,                                                           \
          unlocked->root->root_path.c_str(),                                 \
          strerror(err));                                                    \
    }                                                                        \
    unlocked->root->lock_reason = purpose;                                   \
    /* We've logically moved the callers root into the lock holder */        \
    lock->root = unlocked->root;                                             \
    unlocked->root = nullptr;                                                \
    return true;                                                             \
  }                                                                          \
  void unlocker(lock_type* lock, struct unlocked_watchman_root* unlocked) {  \
    int err;                                                                 \
    /* we need a non-const root local for the read lock case */              \
    w_root_t* root = (w_root_t*)lock->root;                                  \
    if (!root) {                                                             \
      w_log(W_LOG_FATAL, "vacated or already unlocked!\n");                  \
    }                                                                        \
    if (unlocked->root) {                                                    \
      w_log(W_LOG_FATAL, "destination of unlock already holds a root!?\n");  \
    }                                                                        \
    root->lock_reason = nullptr;                                             \
    err = pthread_rwlock_unlock(&root->lock);                                \
    if (err != 0) {                                                          \
      w_log(                                                                 \
          W_LOG_FATAL,                                                       \
          "lock: [%s] %s\n",                                                 \
          lock->root->root_path.c_str(),                                     \
          strerror(err));                                                    \
    }                                                                        \
    unlocked->root = root;                                                   \
    lock->root = nullptr;                                                    \
  }

define_lock_funcs(struct write_locked_watchman_root,
    w_root_lock, pthread_rwlock_wrlock,
    w_root_lock_with_timeout, pthread_rwlock_timedwrlock,
    pthread_rwlock_trywrlock,
    w_root_unlock)

define_lock_funcs(struct read_locked_watchman_root,
    w_root_read_lock, pthread_rwlock_rdlock,
    w_root_read_lock_with_timeout, pthread_rwlock_timedrdlock,
    pthread_rwlock_tryrdlock,
    w_root_read_unlock)

/* vim:ts=2:sw=2:et:
 */
