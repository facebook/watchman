/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "make_unique.h"

static int proc_pid;
static uint64_t proc_start_time;

void w_clockspec_init(void) {
  struct timeval tv;

  proc_pid = (int)getpid();
  if (gettimeofday(&tv, NULL) == -1) {
    w_log(W_LOG_FATAL, "gettimeofday failed: %s\n", strerror(errno));
  }
  proc_start_time = (uint64_t)tv.tv_sec;
}

std::unique_ptr<w_clockspec> w_clockspec_new_clock(
    uint32_t root_number,
    uint32_t ticks) {
  auto spec = watchman::make_unique<w_clockspec>();
  spec->tag = w_cs_clock;
  spec->clock.start_time = proc_start_time;
  spec->clock.pid = proc_pid;
  spec->clock.root_number = root_number;
  spec->clock.ticks = ticks;
  return spec;
}

std::unique_ptr<w_clockspec> w_clockspec_parse(const json_ref& value) {
  const char *str;
  uint64_t start_time;
  int pid;
  uint32_t root_number;
  uint32_t ticks;

  auto spec = watchman::make_unique<w_clockspec>();

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
    spec->clock.root_number = root_number;
    spec->clock.ticks = ticks;
    return spec;
  }

  if (sscanf(str, "c:%d:%" PRIu32, &pid, &ticks) == 2) {
    // old-style clock value (<= 2.8.2) -- by setting clock time and root number
    // to 0 we guarantee that this is treated as a fresh instance
    spec->tag = w_cs_clock;
    spec->clock.start_time = 0;
    spec->clock.pid = pid;
    spec->clock.root_number = root_number;
    spec->clock.ticks = ticks;
    return spec;
  }

  return nullptr;
}

// must be called with the root locked
// spec can be null, in which case a fresh instance is assumed
void w_clockspec_eval(
    struct read_locked_watchman_root* lock,
    const struct w_clockspec* spec,
    struct w_query_since* since) {
  if (spec == NULL) {
    since->is_timestamp = false;
    since->clock.is_fresh_instance = true;
    since->clock.ticks = 0;
    return;
  }

  if (spec->tag == w_cs_timestamp) {
    // just copy the values over
    since->is_timestamp = true;
    since->timestamp = spec->timestamp;
    return;
  }

  since->is_timestamp = false;

  if (spec->tag == w_cs_named_cursor) {
    w_string cursor = spec->named_cursor.cursor;

    {
      // this const_cast is horrible but don't worry, it's just
      // transitional until we eliminate write_locked and read_locked
      // references to w_root_t.
      auto wlock = const_cast<w_root_t*>(lock->root)->inner.cursors.wlock();
      auto& cursors = *wlock;
      auto it = cursors.find(cursor);

      if (it == cursors.end()) {
        since->clock.is_fresh_instance = true;
        since->clock.ticks = 0;
      } else {
        since->clock.ticks = it->second;
        since->clock.is_fresh_instance = since->clock.ticks <
            lock->root->inner.view->getLastAgeOutTickValue();
      }

      // record the current tick value against the cursor so that we use that
      // as the basis for a subsequent query.
      cursors[cursor] = lock->root->inner.view->getMostRecentTickValue();
    }

    w_log(
        W_LOG_DBG,
        "resolved cursor %s -> %" PRIu32 "\n",
        cursor.c_str(),
        since->clock.ticks);
    return;
  }

  // spec->tag == w_cs_clock
  if (spec->clock.start_time == proc_start_time &&
      spec->clock.pid == proc_pid &&
      spec->clock.root_number == lock->root->inner.number) {
    since->clock.is_fresh_instance =
        spec->clock.ticks < lock->root->inner.view->getLastAgeOutTickValue();
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = spec->clock.ticks;
    }
    return;
  }

  // If the pid, start time or root number don't match, they asked a different
  // incarnation of the server or a different instance of this root, so we treat
  // them as having never spoken to us before
  since->clock.is_fresh_instance = true;
  since->clock.ticks = 0;
}

w_clockspec::w_clockspec() : tag(w_cs_timestamp), timestamp(0) {}

w_clockspec::~w_clockspec() {
  if (tag == w_cs_named_cursor) {
    named_cursor.cursor.reset();
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

// Renders the current clock id string to the supplied buffer.
// Must be called with the root locked.
static bool current_clock_id_string(struct read_locked_watchman_root *lock,
                                    char *buf, size_t bufsize) {
  return clock_id_string(
      lock->root->inner.number, lock->root->inner.ticks, buf, bufsize);
}

/* Add the current clock value to the response.
 * must be called with the root locked */
void annotate_with_clock(
    struct read_locked_watchman_root* lock,
    json_ref& resp) {
  char buf[128];

  if (current_clock_id_string(lock, buf, sizeof(buf))) {
    resp.set("clock", typed_string_to_json(buf, W_STRING_UNICODE));
  }
}

/* vim:ts=2:sw=2:et:
 */
