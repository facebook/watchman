/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
#include "watchman_error_category.h"

void handle_open_errno(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* syscall,
    const std::error_code& err) {
  auto dir_name = dir->getFullPath();
  bool log_warning = true;
  bool transient = false;

  if (err == watchman::error_code::no_such_file_or_directory ||
      err == watchman::error_code::not_a_directory ||
      err == watchman::error_code::too_many_symbolic_link_levels) {
    log_warning = false;
    transient = false;
  } else if (err == watchman::error_code::permission_denied) {
    log_warning = true;
    transient = false;
  } else if (err == watchman::error_code::system_limits_exceeded) {
    set_poison_state(dir_name, now, syscall, err);
    return;
  } else {
    log_warning = true;
    transient = true;
  }

  if (w_string_equal(dir_name, root->root_path)) {
    if (!transient) {
      watchman::log(
          watchman::ERR,
          syscall,
          "(",
          dir_name,
          ") -> ",
          err.message(),
          ". Root was deleted; cancelling watch\n");
      root->cancel();
      return;
    }
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
    root->recrawlInfo.wlock()->warning = warn;
  }
}

/* vim:ts=2:sw=2:et:
 */
