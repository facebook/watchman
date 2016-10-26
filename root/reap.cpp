/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool root_has_subscriptions(w_root_t *root) {
  bool has_subscribers = false;

  auto lock = clients.wlock();
  for (auto client_base : *lock) {
    auto client = (watchman_user_client*)client_base;

    for (auto& citer : client->subscriptions) {
      auto sub = citer.second.get();

      if (sub->root == root) {
        has_subscribers = true;
        break;
      }
    }

    if (!has_subscribers) {
      break;
    }
  }
  return has_subscribers;
}

// This is a little tricky.  We have to be called with root->lock
// held, but we must not call w_root_stop_watch with the lock held,
// so we return true if the caller should do that
bool consider_reap(struct read_locked_watchman_root* lock) {
  time_t now;
  auto root = const_cast<w_root_t*>(lock->root);

  if (root->idle_reap_age == 0) {
    return false;
  }

  time(&now);

  if (now > root->inner.last_cmd_timestamp + root->idle_reap_age &&
      (root->triggers.rlock()->empty()) &&
      (now > root->inner.last_reap_timestamp) &&
      !root_has_subscriptions(root)) {
    // We haven't had any activity in a while, and there are no registered
    // triggers or subscriptions against this watch.
    w_log(
        W_LOG_ERR,
        "root %s has had no activity in %d seconds and has "
        "no triggers or subscriptions, cancelling watch.  "
        "Set idle_reap_age_seconds in your .watchmanconfig to control "
        "this behavior\n",
        root->root_path.c_str(),
        root->idle_reap_age);
    return true;
  }

  root->inner.last_reap_timestamp = now;

  return false;
}


/* vim:ts=2:sw=2:et:
 */
