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

#include "watchman/Logging.h"
#include "watchman/WatchmanConfig.h"

namespace watchman {

folly::Synchronized<std::string> poisoned_reason;

void set_poison_state(
    const w_string& dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err) {
  if (!poisoned_reason.rlock()->empty()) {
    return;
  }

  auto why = folly::to<std::string>(
      "A non-recoverable condition has triggered.  Watchman needs your help!\n"
      "The triggering condition was at timestamp=",
      std::chrono::system_clock::to_time_t(now),
      ": ",
      syscall,
      "(",
      dir,
      ") -> ",
      err.message(),
      "\n"
      "All requests will continue to fail with this message until you resolve\n"
      "the underlying problem.  You will find more information on fixing this at\n",
      cfg_get_trouble_url(),
      "#poison-",
      syscall,
      "\n");

  watchman::log(watchman::ERR, why);
  *poisoned_reason.wlock() = why;
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
