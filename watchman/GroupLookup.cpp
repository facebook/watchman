/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/GroupLookup.h"
#include <folly/String.h>
#include "watchman/Logging.h"

#ifndef _WIN32
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <sys/types.h>
#endif

#ifndef _WIN32
using namespace watchman;

const struct group* w_get_group(const char* group_name) {
  // This explicit errno statement is necessary to distinguish between the
  // group not existing and an error.
  errno = 0;
  struct group* group = getgrnam(group_name);
  if (!group) {
    if (errno == 0) {
      logf(ERR, "group '{}' does not exist\n", group_name);
    } else {
      logf(
          ERR,
          "getting gid for '{}' failed: {}\n",
          group_name,
          folly::errnoStr(errno));
    }
    return nullptr;
  }
  return group;
}
#endif // ndef _WIN32
