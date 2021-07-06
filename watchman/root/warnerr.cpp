/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/watchman.h"

using namespace watchman;

void handle_open_errno(
    watchman_root& root,
    struct watchman_dir* dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err) {
  auto dir_name = dir->getFullPath();
  bool log_warning = true;

  if (err == watchman::error_code::no_such_file_or_directory ||
      err == watchman::error_code::not_a_directory ||
      err == watchman::error_code::too_many_symbolic_link_levels) {
    log_warning = false;
  } else if (err == watchman::error_code::permission_denied) {
    log_warning = true;
  } else if (err == watchman::error_code::system_limits_exceeded) {
    set_poison_state(dir_name, now, syscall, err);
    if (!root.failure_reason) {
      root.failure_reason = w_string::build(*poisoned_reason.rlock());
    }
    return;
  } else {
    log_warning = true;
  }

  if (w_string_equal(dir_name, root.root_path)) {
    auto warn = w_string::build(
        syscall,
        "(",
        dir_name,
        ") -> ",
        err.message(),
        ". Root is inaccessible; cancelling watch\n");
    log(ERR, warn);
    if (!root.failure_reason) {
      root.failure_reason = warn;
    }
    root.cancel();
    return;
  }

  auto warn = w_string::build(
      syscall,
      "(",
      dir_name,
      ") -> ",
      err.message(),
      ". Marking this portion of the tree deleted");

  watchman::log(
      err == watchman::error_code::no_such_file_or_directory ? watchman::DBG
                                                             : watchman::ERR,
      warn,
      "\n");
  if (log_warning) {
    root.recrawlInfo.wlock()->warning = warn;
  }
}

/* vim:ts=2:sw=2:et:
 */
