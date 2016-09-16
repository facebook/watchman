/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

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

struct w_clockspec *w_clockspec_new_clock(uint32_t root_number,
                                          uint32_t ticks) {
  auto spec = (w_clockspec*)calloc(1, sizeof(w_clockspec));
  if (!spec) {
    return NULL;
  }
  spec->tag = w_cs_clock;
  spec->clock.start_time = proc_start_time;
  spec->clock.pid = proc_pid;
  spec->clock.root_number = root_number;
  spec->clock.ticks = ticks;
  return spec;
}

struct w_clockspec *w_clockspec_parse(json_t *value) {
  const char *str;
  uint64_t start_time;
  int pid;
  uint32_t root_number;
  uint32_t ticks;

  auto spec = (w_clockspec*)calloc(1, sizeof(w_clockspec));
  if (!spec) {
    return NULL;
  }

  if (json_is_integer(value)) {
    spec->tag = w_cs_timestamp;
    spec->timestamp = (time_t)json_integer_value(value);
    return spec;
  }

  str = json_string_value(value);
  if (!str) {
    free(spec);
    return NULL;
  }

  if (str[0] == 'n' && str[1] == ':') {
    spec->tag = w_cs_named_cursor;
    // spec owns the ref to the string
    spec->named_cursor.cursor = json_to_w_string_incref(value);
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

  free(spec);
  return NULL;
}

void w_clockspec_eval_readonly(struct read_locked_watchman_root *lock,
                               const struct w_clockspec *spec,
                               struct w_query_since *since) {
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
    w_ht_val_t ticks_val;
    w_string_t *cursor = spec->named_cursor.cursor;
    since->clock.is_fresh_instance = !w_ht_lookup(lock->root->cursors,
                                                  w_ht_ptr_val(cursor),
                                                  &ticks_val, false);
    if (!since->clock.is_fresh_instance) {
      since->clock.is_fresh_instance =
          ticks_val < lock->root->last_age_out_tick;
    }
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = (uint32_t)ticks_val;
    }

    w_log(W_LOG_DBG, "resolved cursor %.*s -> %" PRIu32 "\n",
        cursor->len, cursor->buf, since->clock.ticks);
    return;
  }

  // spec->tag == w_cs_clock
  if (spec->clock.start_time == proc_start_time &&
      spec->clock.pid == proc_pid &&
      spec->clock.root_number == lock->root->number) {

    since->clock.is_fresh_instance =
        spec->clock.ticks < lock->root->last_age_out_tick;
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

// must be called with the root locked
// spec can be null, in which case a fresh instance is assumed
void w_clockspec_eval(struct write_locked_watchman_root *lock,
                      const struct w_clockspec *spec,
                      struct w_query_since *since) {
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
    w_ht_val_t ticks_val;
    w_string_t *cursor = spec->named_cursor.cursor;
    since->clock.is_fresh_instance = !w_ht_lookup(lock->root->cursors,
                                                  w_ht_ptr_val(cursor),
                                                  &ticks_val, false);
    if (!since->clock.is_fresh_instance) {
      since->clock.is_fresh_instance =
          ticks_val < lock->root->last_age_out_tick;
    }
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = (uint32_t)ticks_val;
    }

    // Bump the tick value and record it against the cursor.
    // We need to bump the tick value so that repeated queries
    // when nothing has changed in the filesystem won't continue
    // to return the same set of files; we only want the first
    // of these to return the files and the rest to return nothing
    // until something subsequently changes
    w_ht_replace(lock->root->cursors, w_ht_ptr_val(cursor),
                 ++lock->root->ticks);

    w_log(W_LOG_DBG, "resolved cursor %.*s -> %" PRIu32 "\n",
        cursor->len, cursor->buf, since->clock.ticks);
    return;
  }

  // spec->tag == w_cs_clock
  if (spec->clock.start_time == proc_start_time &&
      spec->clock.pid == proc_pid &&
      spec->clock.root_number == lock->root->number) {

    since->clock.is_fresh_instance =
        spec->clock.ticks < lock->root->last_age_out_tick;
    if (since->clock.is_fresh_instance) {
      since->clock.ticks = 0;
    } else {
      since->clock.ticks = spec->clock.ticks;
    }
    if (spec->clock.ticks == lock->root->ticks) {
      /* Force ticks to increment.  This avoids returning and querying the
       * same tick value over and over when no files have changed in the
       * meantime */
      lock->root->ticks++;
    }
    return;
  }

  // If the pid, start time or root number don't match, they asked a different
  // incarnation of the server or a different instance of this root, so we treat
  // them as having never spoken to us before
  since->clock.is_fresh_instance = true;
  since->clock.ticks = 0;
}

void w_clockspec_free(struct w_clockspec *spec) {
  if (spec->tag == w_cs_named_cursor) {
    w_string_delref(spec->named_cursor.cursor);
  }
  free(spec);
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
  return clock_id_string(lock->root->number, lock->root->ticks, buf, bufsize);
}

/* Add the current clock value to the response.
 * must be called with the root locked */
void annotate_with_clock(struct read_locked_watchman_root *lock, json_t *resp) {
  char buf[128];

  if (current_clock_id_string(lock, buf, sizeof(buf))) {
    set_prop(resp, "clock", typed_string_to_json(buf, W_STRING_UNICODE));
  }
}

/* vim:ts=2:sw=2:et:
 */
