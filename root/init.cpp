/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "InMemoryView.h"
#include "make_unique.h"

static json_ref load_root_config(const char* path) {
  char cfgfilename[WATCHMAN_NAME_MAX];
  json_error_t err;

  snprintf(cfgfilename, sizeof(cfgfilename), "%s/.watchmanconfig", path);

  if (!w_path_exists(cfgfilename)) {
    if (errno == ENOENT) {
      return nullptr;
    }
    w_log(W_LOG_ERR, "%s is not accessible: %s\n",
        cfgfilename, strerror(errno));
    return nullptr;
  }

  auto res = json_load_file(cfgfilename, 0, &err);
  if (!res) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
        cfgfilename, err.text);
  }
  return res;
}

void watchman_root::applyIgnoreConfiguration() {
  uint8_t i;
  json_t *ignores;

  ignores = config.get("ignore_dirs");
  if (!ignores) {
    return;
  }
  if (!json_is_array(ignores)) {
    w_log(W_LOG_ERR, "ignore_dirs must be an array of strings\n");
    return;
  }

  for (i = 0; i < json_array_size(ignores); i++) {
    auto jignore = json_array_get(ignores, i);

    if (!json_is_string(jignore)) {
      w_log(W_LOG_ERR, "ignore_dirs must be an array of strings\n");
      continue;
    }

    auto name = json_to_w_string(jignore);
    auto fullname = w_string::pathCat({root_path, name});
    ignore.add(fullname, false);
    w_log(W_LOG_DBG, "ignoring %s recursively\n", fullname.c_str());
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

  time(&inner.last_cmd_timestamp);
}

void watchman_root::Inner::init(w_root_t* root) {
  view_ = WatcherRegistry::initWatcher(root);
}

watchman_root::watchman_root(const w_string& root_path)
    : root_path(root_path),
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
  w_log(W_LOG_DBG, "root: final ref on %s\n", root_path.c_str());
  --live_roots;
}

/* vim:ts=2:sw=2:et:
 */
