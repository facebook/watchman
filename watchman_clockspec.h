/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_clock {
  uint32_t ticks;
  time_t timestamp;
};
typedef struct watchman_clock w_clock_t;

enum w_clockspec_tag {
  w_cs_timestamp,
  w_cs_clock,
  w_cs_named_cursor
};

struct w_clockspec {
  enum w_clockspec_tag tag;
  union {
    time_t timestamp;
    struct {
      uint64_t start_time;
      int pid;
      uint32_t root_number;
      uint32_t ticks;
    } clock;
    struct {
      w_string_t *cursor;
    } named_cursor;
  };
};

struct w_clockspec *w_clockspec_new_clock(uint32_t root_number, uint32_t ticks);
struct w_clockspec *w_clockspec_parse(json_t *value);
void w_clockspec_eval(struct write_locked_watchman_root *lock,
                      const struct w_clockspec *spec,
                      struct w_query_since *since);
void w_clockspec_eval_readonly(struct read_locked_watchman_root *lock,
                               const struct w_clockspec *spec,
                               struct w_query_since *since);
void w_clockspec_free(struct w_clockspec *spec);
void w_clockspec_init(void);
