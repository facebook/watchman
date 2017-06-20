/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include <exception>
#include "watchman.h"

namespace watchman {

CookieSync::Cookie::Cookie(w_string name) : fileName(name) {}

CookieSync::Cookie::~Cookie() {
  // The file may not exist at this point; we're just taking this
  // opportunity to remove it if nothing else has done so already.
  // We don't care about the return code; best effort is fine.
  unlink(fileName.c_str());
}

CookieSync::CookieSync(const w_string& dir) {
  setCookieDir(dir);
}

CookieSync::~CookieSync() {
  // Wake up anyone that might have been waiting on us
  abortAllCookies();
}

void CookieSync::setCookieDir(const w_string& dir) {
  cookieDir_ = dir;

  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  hostname[sizeof(hostname) - 1] = '\0';

  cookiePrefix_ = w_string::printf(
      "%.*s/" WATCHMAN_COOKIE_PREFIX "%s-%d-",
      int(cookieDir_.size()),
      cookieDir_.data(),
      hostname,
      int(getpid()));
}

Future<Unit> CookieSync::sync() {
  /* generate a cookie name: cookie prefix + id */
  auto path_str = w_string::printf(
      "%.*s%" PRIu32,
      int(cookiePrefix_.size()),
      cookiePrefix_.data(),
      serial_++);

  auto cookie = make_unique<Cookie>(path_str);
  auto future = cookie->promise.getFuture();

  /* insert our cookie in the map */
  {
    auto wlock = cookies_.wlock();
    auto& map = *wlock;
    map[path_str] = std::move(cookie);
  }

  /* then touch the file */
  auto file = w_stm_open(
      path_str.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700);
  if (!file) {
    auto errcode = errno;
    // The erase will unlink the file
    cookies_.wlock()->erase(path_str);

    throw std::system_error(
        errcode,
        std::generic_category(),
        to<std::string>(
            "sync: creat(", path_str, ") failed: ", strerror(errcode)));
  }
  log(DBG, "sync created cookie file ", path_str, "\n");
  return future;
}

bool CookieSync::syncToNow(std::chrono::milliseconds timeout) {
  /* compute deadline */
  using namespace std::chrono;
  auto deadline = system_clock::now() + timeout;

  while (true) {
    auto cookie = sync();

    if (!cookie.wait_for(timeout)) {
      log(ERR,
          "syncToNow: timed out waiting for cookie file to be "
          "observed by watcher within ",
          timeout.count(),
          " milliseconds\n");
      errno = ETIMEDOUT;
      return false;
    }

    if (cookie.result().hasError()) {
      // Sync was aborted by a recrawl; recompute the timeout
      // and wait again if we still have time
      timeout = duration_cast<milliseconds>(deadline - system_clock::now());
      if (timeout.count() <= 0) {
        errno = ETIMEDOUT;
        return false;
      }

      // wait again
      continue;
    }

    // Success!
    return true;
  }
}

void CookieSync::abortAllCookies() {
  std::unordered_map<w_string, std::unique_ptr<Cookie>> cookies;

  {
    auto map = cookies_.wlock();
    std::swap(*map, cookies);
  }

  for (auto& it : cookies) {
    log(ERR, "syncToNow: aborting cookie ", it.first, "\n");
    it.second->promise.setException(
        std::make_exception_ptr(CookieSyncAborted()));
  }
}

void CookieSync::notifyCookie(const w_string& path) {
  std::unique_ptr<Cookie> cookie;

  {
    auto map = cookies_.wlock();
    auto cookie_iter = map->find(path);
    log(DBG,
        "cookie for ",
        path,
        "? ",
        cookie_iter != map->end() ? "yes" : "no",
        "\n");

    if (cookie_iter != map->end()) {
      cookie = std::move(cookie_iter->second);
      map->erase(cookie_iter);
    }
  }

  if (cookie) {
    cookie->promise.setValue(Unit{});
    // cookie file will be unlinked when we exit this scope
  }
}
}
