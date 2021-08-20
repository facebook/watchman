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

#pragma once
#include <folly/Synchronized.h>
#include <folly/futures/Future.h>
#include "watchman/Cookie.h"
#include "watchman/watchman_string.h"

namespace watchman {

class CookieSyncAborted : public std::exception {};

class CookieSync {
 public:
  explicit CookieSync(const w_string& dir);
  ~CookieSync();

  void setCookieDir(const w_string& dir);
  void addCookieDir(const w_string& dir);
  void removeCookieDir(const w_string& dir);

  /**
   * Ensure that we're synchronized with the state of the
   * filesystem at the current time.
   *
   * We do this by touching one or more cookie files and waiting to
   * observe them via the watcher.  When we see it we know that
   * we've seen everything up to the point in time at which
   * we're asking questions. (Note: it's unclear if all filesystem watchers
   * provide such an ordering guarantee, and it's worth flushing all pending
   * notifications to be sure.)
   *
   * Throws a std::system_error with an ETIMEDOUT
   * error if the timeout expires before we observe the change, or a
   * runtime_error if the root has been deleted or rendered inaccessible.
   *
   * cookieFileNames is populated with the names of all cookie files used.
   */
  void syncToNow(
      std::chrono::milliseconds timeout,
      std::vector<w_string>& cookieFileNames);

  /**
   * Touches a cookie file and returns a Future that will
   * be ready when that cookie file is processed by the IO
   * thread at some future time.
   * Important: if you chain a lambda onto the future, it
   * will execute in the context of the IO thread.
   * It is recommended that you minimize the actions performed
   * in that context to avoid holding up the IO thread.
   **/
  folly::Future<folly::Unit> sync(std::vector<w_string>& cookieFileNames);

  /* If path is a valid cookie in the map, notify the waiter.
   * Returns true if the path matches the cookie prefix (not just
   * whether the cookie is currently valid).
   * Returns false if the path does not match our cookie prefix.
   */
  void notifyCookie(const w_string& path);

  /* Cause all pending cookie sync promises to complete immediately
   * with a CookieSyncAborted exception */
  void abortAllCookies();

  // Check if this path matches an actual cookie.
  bool isCookiePrefix(w_string_piece path);

  // Check if the path matches a cookie directory.
  bool isCookieDir(w_string_piece path);

  // Returns the set of prefixes for cookie files
  std::unordered_set<w_string> cookiePrefix() const;

  std::unordered_set<w_string> cookieDirs() const;

  // Returns the list of cookies that are pending observation; each of
  // these has an associated waiting client.
  std::vector<w_string> getOutstandingCookieFileList() const;

 private:
  struct Cookie {
    folly::Promise<folly::Unit> promise;
    std::atomic<uint64_t> numPending;

    explicit Cookie(uint64_t numCookies);

    void notify();
  };

  struct CookieDirectories {
    // paths to the query cookies directories. A cookie will be written to each
    // of these when calling `sync`.
    std::unordered_set<w_string> dirs_;
    // valid filename prefix for cookies we create
    w_string cookiePrefix_;
  };

  folly::Synchronized<CookieDirectories> cookieDirs_;
  // Serial number for cookie filename
  std::atomic<uint32_t> serial_{0};
  using CookieMap = std::unordered_map<w_string, std::shared_ptr<Cookie>>;
  folly::Synchronized<CookieMap> cookies_;
};
} // namespace watchman
