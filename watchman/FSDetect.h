#pragma once

#include <folly/Range.h>
#include "watchman/watchman_string.h"

// Returns the name of the filesystem for the specified path
w_string w_fstype(const char* path);
w_string find_fstype_in_linux_proc_mounts(
    folly::StringPiece path,
    folly::StringPiece procMountsData);

inline bool is_edenfs_fs_type(w_string_piece fs_type) {
  return fs_type == "edenfs" || fs_type.startsWith("edenfs:");
}
