/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::shared_ptr<watchman::QueryableView> watchman_root::view() {
  // We grab a read lock on the recrawl info to ensure that we
  // can't race with scheduleRecrawl and observe a nullptr for
  // the view_.
  auto info = recrawlInfo.rlock();
  return inner.view_;
}

void watchman_root::scheduleRecrawl(const char* why) {
  {
    auto info = recrawlInfo.wlock();

    if (!info->shouldRecrawl) {
      if (!config.getBool("suppress_recrawl_warnings", false)) {
        info->warning = w_string::build(
            "Recrawled this watch ",
            ++info->recrawlCount,
            " times, most recently because:\n",
            why,
            "To resolve, please review the information on\n",
            cfg_get_trouble_url(),
            "#recrawl");
      }

      watchman::log(
          watchman::ERR, root_path, ": ", why, ": scheduling a tree recrawl\n");
    }
    info->shouldRecrawl = true;
  }
  view()->wakeThreads();
}

void watchman_root::signalThreads() {
  view()->signalThreads();
}

// Cancels a watch.
bool watchman_root::cancel() {
  bool cancelled = false;

  if (!inner.cancelled) {
    cancelled = true;

    watchman::log(watchman::DBG, "marked ", root_path, " cancelled\n");
    inner.cancelled = true;

    // The client will fan this out to all matching subscriptions.
    // This happens in listener.cpp.
    unilateralResponses->enqueue(json_object(
        {{"root", w_string_to_json(root_path)}, {"canceled", json_true()}}));

    signalThreads();
    removeFromWatched();

    {
      auto map = triggers.rlock();
      for (auto& it : *map) {
        it.second->stop();
      }
    }
  }

  return cancelled;
}

bool watchman_root::stopWatch() {
  bool stopped = removeFromWatched();

  if (stopped) {
    cancel();
    w_state_save();
  }
  signalThreads();

  return stopped;
}

/* vim:ts=2:sw=2:et:
 */
