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
      "%.*s%c" WATCHMAN_COOKIE_PREFIX "%s-%d-",
      int(cookieDir_.size()),
      cookieDir_.data(),
      WATCHMAN_DIR_SEP,
      hostname,
      int(getpid()));
}

bool CookieSync::syncToNow(std::chrono::milliseconds timeout) {
  Cookie cookie;
  w_stm_t file;
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
  file = w_stm_open(
      path_str.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700);
  if (!file) {
    errcode = errno;
    w_log(
        W_LOG_ERR,
        "sync_to_now: creat(%s) failed: %s\n",
        path_str.c_str(),
        strerror(errcode));
    goto out;
  }
  w_stm_close(file);

  w_log(W_LOG_DBG, "sync_to_now [%s] waiting\n", path_str.c_str());

  /* timed cond wait (unlocks cookie lock, reacquires) */
  if (!cookie.cond.wait_until(
          cookie_lock, deadline, [&] { return cookie.seen; })) {
    w_log(
        W_LOG_ERR,
        "sync_to_now: %s timedwait failed: %d: istimeout=%d %s\n",
        path_str.c_str(),
        errcode,
        errcode == ETIMEDOUT,
        strerror(errcode));
    goto out;
  }
  w_log(W_LOG_DBG, "sync_to_now [%s] done\n", path_str.c_str());

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
    cookie->seen = true;
    cookie->cond.notify_one();
  }
}
}
