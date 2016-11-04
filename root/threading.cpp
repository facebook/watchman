/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool watchman_root::start(char**) {
  inner.view->startThreads(this);
  return true;
}

void watchman_root::scheduleRecrawl(const char* why) {
  auto info = recrawlInfo.wlock();

  if (!info->shouldRecrawl) {
    info->lastRecrawlReason = w_string::build(root_path, ": ", why);

    watchman::log(
        watchman::ERR, root_path, ": ", why, ": scheduling a tree recrawl\n");
  }
  info->shouldRecrawl = true;
  signalThreads();
}

void watchman_root::signalThreads() {
  inner.view->signalThreads();
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
