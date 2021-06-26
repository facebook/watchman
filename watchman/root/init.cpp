/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/watchman.h"

#include <folly/String.h>
#include "watchman/InMemoryView.h"
#include "watchman/watcher/WatcherRegistry.h"

using namespace watchman;

static json_ref load_root_config(const char* path) {
  char cfgfilename[WATCHMAN_NAME_MAX];

  snprintf(cfgfilename, sizeof(cfgfilename), "%s/.watchmanconfig", path);

  if (!w_path_exists(cfgfilename)) {
    if (errno == ENOENT) {
      return nullptr;
    }
    logf(
        ERR, "{} is not accessible: {}\n", cfgfilename, folly::errnoStr(errno));
    return nullptr;
  }

  return json_load_file(cfgfilename, 0);
}

void watchman_root::applyIgnoreConfiguration() {
  auto ignores = config.get("ignore_dirs");
  if (!ignores) {
    return;
  }
  if (!ignores.isArray()) {
    logf(ERR, "ignore_dirs must be an array of strings\n");
    return;
  }

  for (size_t i = 0; i < json_array_size(ignores); i++) {
    auto jignore = json_array_get(ignores, i);

    if (!jignore.isString()) {
      logf(ERR, "ignore_dirs must be an array of strings\n");
      continue;
    }

    auto name = json_to_w_string(jignore);
    auto fullname = w_string::pathCat({root_path, name});
    ignore.add(fullname, false);
    logf(DBG, "ignoring {} recursively\n", fullname);
  }
}

// internal initialization for root
void watchman_root::init() {
  // This just opens and releases the dir.  If an exception is thrown
  // it will bubble up.
  w_dir_open(root_path.c_str());
  // We can't use shared_from_this() here as we are being called from
  // inside the constructor and we'd hit a bad_weak_ptr exception.
  inner.init(this);

  inner.last_cmd_timestamp = std::chrono::steady_clock::now();
}

void watchman_root::Inner::init(watchman_root* root) {
  view_ = WatcherRegistry::initWatcher(root);
}

watchman_root::watchman_root(const w_string& root_path, const w_string& fs_type)
    : root_path(root_path),
      fs_type(fs_type),
      case_sensitive(watchman::getCaseSensitivityForPath(root_path.c_str())),
      cookies(root_path),
      config_file(load_root_config(root_path.c_str())),
      config(config_file),
      trigger_settle(int(config.getInt("settle", DEFAULT_SETTLE_PERIOD))),
      gc_interval(
          int(config.getInt("gc_interval_seconds", DEFAULT_GC_INTERVAL))),
      gc_age(int(config.getInt("gc_age_seconds", DEFAULT_GC_AGE))),
      idle_reap_age(
          int(config.getInt("idle_reap_age_seconds", DEFAULT_REAP_AGE))),
      unilateralResponses(std::make_shared<watchman::Publisher>()) {
  ++live_roots;
  applyIgnoreConfiguration();
  applyIgnoreVCSConfiguration();
  init();
}

watchman_root::~watchman_root() {
  logf(DBG, "root: final ref on {}\n", root_path);
  --live_roots;
}

/* vim:ts=2:sw=2:et:
 */
