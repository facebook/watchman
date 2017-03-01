/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <unordered_map>
#include "watchman_synchronized.h"

struct watchman_clock {
  uint32_t ticks;
  time_t timestamp;
};
typedef struct watchman_clock w_clock_t;

struct w_query_ctx;
struct w_query_since;

struct ClockPosition {
  uint32_t rootNumber{0};
  uint32_t ticks{0};

  ClockPosition() = default;
  ClockPosition(uint32_t rootNumber, uint32_t ticks)
      : rootNumber(rootNumber), ticks(ticks) {}

  w_string toClockString() const;
};

enum w_clockspec_tag {
  w_cs_timestamp,
  w_cs_clock,
  w_cs_named_cursor
};

struct ClockSpec {
  enum w_clockspec_tag tag;
  time_t timestamp;
  struct {
    uint64_t start_time;
    int pid;
    ClockPosition position;
  } clock;
  struct {
    w_string cursor;
  } named_cursor;

  // Optional SCM merge base parameters
  w_string scmMergeBase;
  w_string scmMergeBaseWith;

  ClockSpec();
  explicit ClockSpec(const ClockPosition& position);
  explicit ClockSpec(const json_ref& value);

  /** Given a json value, parse out a clockspec.
   * Will return nullptr if the input was json null, indicating
   * an absence of a specified clock value.
   * Throws std::domain_error for badly formed clockspec value.
   */
  static std::unique_ptr<ClockSpec> parseOptionalClockSpec(
      const json_ref& value);

  /** Evaluate the clockspec against the inputs, returning
   * the effective since parameter.
   * If cursorMap is passed in, it MUST be unlocked, as this method
   * will acquire a lock to evaluate a named cursor. */
  struct w_query_since evaluate(
      const ClockPosition& position,
      const uint32_t lastAgeOutTick,
      watchman::Synchronized<std::unordered_map<w_string, uint32_t>>*
          cursorMap = nullptr) const;

  /** Initializes some global state needed for clockspec evaluation */
  static void init();

  inline const ClockPosition& position() const {
    w_check(tag == w_cs_clock, "position() called for non-clock clockspec");
    return clock.position;
  }

  bool hasScmParams() const;

  /** Returns a json value representing the current state of this ClockSpec
   * that can be parsed by the ClockSpec(const json_ref&)
   * constructor of this class */
  json_ref toJson() const;
};
