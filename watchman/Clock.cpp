/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Clock.h"
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <memory>

using namespace watchman;

static int proc_pid;
static uint64_t proc_start_time;

void ClockSpec::init() {
  struct timeval tv;

  proc_pid = (int)::getpid();
  if (gettimeofday(&tv, NULL) == -1) {
    logf(FATAL, "gettimeofday failed: {}\n", folly::errnoStr(errno));
  }
  proc_start_time = (uint64_t)tv.tv_sec;
}

ClockSpec::ClockSpec(const json_ref& value) {
  auto parseClockString = [=](const char* str) {
    uint64_t start_time;
    int pid;
    uint32_t root_number;
    uint32_t ticks;
    // Parse a >= 2.8.2 version clock string
    if (sscanf(
            str,
            "c:%" PRIu64 ":%d:%" PRIu32 ":%" PRIu32,
            &start_time,
            &pid,
            &root_number,
            &ticks) == 4) {
      tag = w_cs_clock;
      clock.start_time = start_time;
      clock.pid = pid;
      clock.position.rootNumber = root_number;
      clock.position.ticks = ticks;
      return true;
    }

    if (sscanf(str, "c:%d:%" PRIu32, &pid, &ticks) == 2) {
      // old-style clock value (<= 2.8.2) -- by setting clock time and root
      // number to 0 we guarantee that this is treated as a fresh instance
      tag = w_cs_clock;
      clock.start_time = 0;
      clock.pid = pid;
      clock.position.rootNumber = root_number;
      clock.position.ticks = ticks;
      return true;
    }

    return false;
  };

  switch (json_typeof(value)) {
    case JSON_INTEGER:
      tag = w_cs_timestamp;
      timestamp = (time_t)value.asInt();
      return;

    case JSON_OBJECT: {
      auto clockStr = value.get_default("clock");
      if (clockStr) {
        if (!parseClockString(json_string_value(clockStr))) {
          throw std::domain_error("invalid clockspec");
        }
      } else {
        tag = w_cs_clock;
        clock.start_time = 0;
        clock.pid = 0;
        clock.position.rootNumber = 0;
        clock.position.ticks = 0;
      }

      auto scm = value.get_default("scm");
      if (scm) {
        scmMergeBase = json_to_w_string(
            scm.get_default("mergebase", w_string_to_json("")));
        scmMergeBaseWith = json_to_w_string(scm.get("mergebase-with"));
        auto savedState = scm.get_default("saved-state");
        if (savedState) {
          savedStateConfig = savedState.get("config");
          savedStateStorageType = json_to_w_string(savedState.get("storage"));
          auto commitId = savedState.get_default("commit-id");
          if (commitId) {
            savedStateCommitId = json_to_w_string(commitId);
          } else {
            savedStateCommitId = w_string();
          }
        }
      }

      return;
    }

    case JSON_STRING: {
      auto str = json_string_value(value);

      if (str[0] == 'n' && str[1] == ':') {
        tag = w_cs_named_cursor;
        named_cursor.cursor = json_to_w_string(value);
        return;
      }

      if (parseClockString(str)) {
        return;
      }

      /* fall through to default case and throw error.
       * The redundant looking comment below is a hint to
       * gcc that it is ok to fall through. */
    }
      /* fall through */

    default:
      throw std::domain_error("invalid clockspec");
  }
}

std::unique_ptr<ClockSpec> ClockSpec::parseOptionalClockSpec(
    const json_ref& value) {
  if (value.isNull()) {
    return nullptr;
  }
  return std::make_unique<ClockSpec>(value);
}

ClockSpec::ClockSpec() : tag(w_cs_timestamp), timestamp(0) {}

ClockSpec::ClockSpec(const ClockPosition& position)
    : tag(w_cs_clock), clock{proc_start_time, proc_pid, position} {}

QuerySince ClockSpec::evaluate(
    const ClockPosition& position,
    const uint32_t lastAgeOutTick,
    folly::Synchronized<std::unordered_map<w_string, uint32_t>>* cursorMap)
    const {
  QuerySince since;

  switch (tag) {
    case w_cs_timestamp:
      // just copy the values over
      since.is_timestamp = true;
      since.timestamp = timestamp;
      return since;

    case w_cs_named_cursor: {
      if (!cursorMap) {
        // This is checked for and handled at parse time in SinceExpr::parse,
        // so this should be impossible to hit.
        throw std::runtime_error(
            "illegal to use a named cursor in this context");
      }

      {
        auto wlock = cursorMap->wlock();
        auto& cursors = *wlock;
        auto it = cursors.find(named_cursor.cursor);

        if (it == cursors.end()) {
          since.clock.is_fresh_instance = true;
          since.clock.ticks = 0;
        } else {
          since.clock.ticks = it->second;
          since.clock.is_fresh_instance = since.clock.ticks < lastAgeOutTick;
        }

        // record the current tick value against the cursor so that we use that
        // as the basis for a subsequent query.
        cursors[named_cursor.cursor] = position.ticks;
      }

      watchman::log(
          watchman::DBG,
          "resolved cursor ",
          named_cursor.cursor,
          " -> ",
          since.clock.ticks,
          "\n");

      return since;
    }

    case w_cs_clock: {
      if (clock.start_time == proc_start_time && clock.pid == proc_pid &&
          clock.position.rootNumber == position.rootNumber) {
        since.clock.is_fresh_instance = clock.position.ticks < lastAgeOutTick;
        if (since.clock.is_fresh_instance) {
          since.clock.ticks = 0;
        } else {
          since.clock.ticks = clock.position.ticks;
        }
      } else {
        // If the pid, start time or root number don't match, they asked a
        // different incarnation of the server or a different instance of this
        // root, so we treat them as having never spoken to us before.
        since.clock.is_fresh_instance = true;
        since.clock.ticks = 0;
      }
      return since;
    }

    default:
      throw std::runtime_error("impossible case in ClockSpec::evaluate");
  }
}

bool clock_id_string(
    uint32_t root_number,
    uint32_t ticks,
    char* buf,
    size_t bufsize) {
  int res = snprintf(
      buf,
      bufsize,
      "c:%" PRIu64 ":%d:%u:%" PRIu32,
      proc_start_time,
      proc_pid,
      root_number,
      ticks);

  if (res == -1) {
    return false;
  }
  return (size_t)res < bufsize;
}

w_string ClockPosition::toClockString() const {
  char clockbuf[128];
  if (!clock_id_string(rootNumber, ticks, clockbuf, sizeof(clockbuf))) {
    throw std::runtime_error("clock is too big for clockbuf");
  }
  return w_string(clockbuf, W_STRING_UNICODE);
}

json_ref ClockSpec::toJson() const {
  if (hasScmParams()) {
    auto scm = json_object(
        {{"mergebase", w_string_to_json(scmMergeBase)},
         {"mergebase-with", w_string_to_json(scmMergeBaseWith)}});
    if (hasSavedStateParams()) {
      auto savedState = json_object(
          {{"storage", w_string_to_json(savedStateStorageType)},
           {"config", savedStateConfig}});
      if (savedStateCommitId != w_string()) {
        json_object_set(
            savedState, "commit-id", w_string_to_json(savedStateCommitId));
      }
      json_object_set(scm, "saved-state", savedState);
    }
    return json_object(
        {{"clock", w_string_to_json(position().toClockString())},
         {"scm", scm}});
  }
  return w_string_to_json(position().toClockString());
}

bool ClockSpec::hasScmParams() const {
  return scmMergeBase;
}

bool ClockSpec::hasSavedStateParams() const {
  return savedStateStorageType;
}

/* vim:ts=2:sw=2:et:
 */
