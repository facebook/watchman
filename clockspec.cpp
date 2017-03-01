/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "make_unique.h"

static int proc_pid;
static uint64_t proc_start_time;

void ClockSpec::init() {
  struct timeval tv;

  proc_pid = (int)getpid();
  if (gettimeofday(&tv, NULL) == -1) {
    w_log(W_LOG_FATAL, "gettimeofday failed: %s\n", strerror(errno));
  }
  proc_start_time = (uint64_t)tv.tv_sec;
}

std::unique_ptr<ClockSpec> w_clockspec_parse(const json_ref& value) {
  const char *str;
  uint64_t start_time;
  int pid;
  uint32_t root_number;
  uint32_t ticks;

  auto spec = watchman::make_unique<ClockSpec>();

  if (json_is_integer(value)) {
    spec->tag = w_cs_timestamp;
    spec->timestamp = (time_t)json_integer_value(value);
    return spec;
  }

  str = json_string_value(value);
  if (!str) {
    return nullptr;
  }

  if (str[0] == 'n' && str[1] == ':') {
    spec->tag = w_cs_named_cursor;
    // spec owns the ref to the string
    spec->named_cursor.cursor = json_to_w_string(value);
    return spec;
  }

  if (sscanf(str, "c:%" PRIu64 ":%d:%" PRIu32 ":%" PRIu32,
             &start_time, &pid, &root_number, &ticks) == 4) {
    spec->tag = w_cs_clock;
    spec->clock.start_time = start_time;
    spec->clock.pid = pid;
    spec->clock.position.rootNumber = root_number;
    spec->clock.position.ticks = ticks;
    return spec;
  }

  if (sscanf(str, "c:%d:%" PRIu32, &pid, &ticks) == 2) {
    // old-style clock value (<= 2.8.2) -- by setting clock time and root number
    // to 0 we guarantee that this is treated as a fresh instance
    spec->tag = w_cs_clock;
    spec->clock.start_time = 0;
    spec->clock.pid = pid;
    spec->clock.position.rootNumber = root_number;
    spec->clock.position.ticks = ticks;
    return spec;
  }

  return nullptr;
}

ClockSpec::ClockSpec() : tag(w_cs_timestamp), timestamp(0) {}

ClockSpec::~ClockSpec() {
  if (tag == w_cs_named_cursor) {
    named_cursor.cursor.reset();
  }
}

ClockSpec::ClockSpec(const ClockPosition& position)
    : tag(w_cs_clock), clock{proc_start_time, proc_pid, position} {}

w_query_since ClockSpec::evaluate(
    const ClockPosition& position,
    const uint32_t lastAgeOutTick,
    watchman::Synchronized<std::unordered_map<w_string, uint32_t>>* cursorMap)
    const {
  w_query_since since;

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

bool clock_id_string(uint32_t root_number, uint32_t ticks, char *buf,
                     size_t bufsize) {
  int res = snprintf(buf, bufsize, "c:%" PRIu64 ":%d:%u:%" PRIu32,
                     proc_start_time, proc_pid, root_number, ticks);

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

/* Add the current clock value to the response */
void annotate_with_clock(
    const std::shared_ptr<w_root_t>& root,
    json_ref& resp) {
  resp.set(
      "clock", w_string_to_json(root->inner.view->getCurrentClockString()));
}

/* vim:ts=2:sw=2:et:
 */
