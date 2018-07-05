/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "watchman_log.h"
#ifndef _WIN32
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <sys/types.h>
#endif

#ifndef _WIN32
const struct group* w_get_group(const char* group_name) {
  // This explicit errno statement is necessary to distinguish between the
  // group not existing and an error.
  errno = 0;
  struct group* group = getgrnam(group_name);
  if (!group) {
    if (errno == 0) {
      w_log(W_LOG_ERR, "group '%s' does not exist\n", group_name);
    } else {
      w_log(
          W_LOG_ERR,
          "getting gid for '%s' failed: %s\n",
          group_name,
          strerror(errno));
    }
    return nullptr;
  }
  return group;
}
#endif // ndef _WIN32
