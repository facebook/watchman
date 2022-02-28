/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/Synchronized.h>
#include <unordered_map>
#include "watchman/Logging.h"

namespace watchman {

using ClockTicks = uint32_t;

struct ClockStamp {
  ClockTicks ticks;
  time_t timestamp;
};

struct QuerySince {
  bool is_timestamp;
  union {
    time_t timestamp;
    struct {
      bool is_fresh_instance;
      ClockTicks ticks;
    } clock;
  };

  QuerySince() : is_timestamp(false), clock{true, 0} {}
};

struct ClockPosition {
  uint32_t rootNumber{0};
  ClockTicks ticks{0};

  ClockPosition() = default;
  ClockPosition(uint32_t rootNumber, ClockTicks ticks)
      : rootNumber(rootNumber), ticks(ticks) {}

  w_string toClockString() const;
};

enum w_clockspec_tag { w_cs_timestamp, w_cs_clock, w_cs_named_cursor };

struct ClockSpec {
  w_clockspec_tag tag;
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
  // Optional saved state parameters
  json_ref savedStateConfig;
  w_string savedStateStorageType;
  w_string savedStateCommitId;

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
  QuerySince evaluate(
      const ClockPosition& position,
      const ClockTicks lastAgeOutTick,
      folly::Synchronized<std::unordered_map<w_string, ClockTicks>>* cursorMap =
          nullptr) const;

  /** Initializes some global state needed for clockspec evaluation */
  static void init();

  inline const ClockPosition& position() const {
    w_check(tag == w_cs_clock, "position() called for non-clock clockspec");
    return clock.position;
  }

  bool hasScmParams() const;
  bool hasSavedStateParams() const;

  /** Returns a json value representing the current state of this ClockSpec
   * that can be parsed by the ClockSpec(const json_ref&)
   * constructor of this class */
  json_ref toJson() const;
};

} // namespace watchman

bool clock_id_string(
    uint32_t root_number,
    watchman::ClockTicks ticks,
    char* buf,
    size_t bufsize);
