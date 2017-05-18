/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

namespace watchman {

CookieSync::CookieSync(const w_string& dir) {
  setCookieDir(dir);
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

bool CookieSync::syncToNow(std::chrono::milliseconds timeout) {
  Cookie cookie;
  int errcode = 0;

  auto cookie_lock = std::unique_lock<std::mutex>(cookie.mutex);

  /* generate a cookie name: cookie prefix + id */
  auto path_str = w_string::printf(
      "%.*s%" PRIu32,
      int(cookiePrefix_.size()),
      cookiePrefix_.data(),
      serial_++);

  /* insert our cookie in the map */
  {
    auto wlock = cookies_.wlock();
    auto& map = *wlock;
    map[path_str] = &cookie;
  }

  /* compute deadline */
  auto deadline = std::chrono::system_clock::now() + timeout;

  /* touch the file */
  auto file = w_stm_open(
      path_str.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700);
  if (!file) {
    errcode = errno;
    log(ERR,
        "syncToNow: creat(",
        path_str,
        ") failed: ",
        strerror(errcode),
        "\n");
    goto out;
  }
  file.reset();

  log(DBG, "syncToNow [", path_str, "] waiting\n");

  /* timed cond wait (unlocks cookie lock, reacquires) */
  if (!cookie.cond.wait_until(
          cookie_lock, deadline, [&] { return cookie.seen; })) {
    log(ERR,
        "syncToNow: timed out waiting for cookie file ",
        path_str,
        " to be observed by watcher within ",
        timeout.count(),
        " milliseconds\n");
    errcode = ETIMEDOUT;
    goto out;
  }
  log(DBG, "syncToNow [", path_str, "] done\n");

out:
  cookie_lock.unlock();

  // can't unlink the file until after the cookie has been observed because
  // we don't know which file got changed until we look in the cookie dir
  unlink(path_str.c_str());

  {
    auto map = cookies_.wlock();
    map->erase(path_str);
  }

  if (!cookie.seen) {
    errno = errcode;
    return false;
  }

  return true;
}

void CookieSync::notifyCookie(const w_string& path) const {
  auto map = cookies_.rlock();
  auto cookie_iter = map->find(path);
  w_log(
      W_LOG_DBG,
      "cookie for %s? %s\n",
      path.c_str(),
      cookie_iter != map->end() ? "yes" : "no");

  if (cookie_iter != map->end()) {
    auto cookie = cookie_iter->second;
    auto cookie_lock = std::unique_lock<std::mutex>(cookie->mutex);
    cookie->seen = true;
    cookie->cond.notify_one();
  }
}
}
