/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static json_t *global_cfg = NULL;
static json_t *arg_cfg = NULL;
static pthread_rwlock_t cfg_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Called during shutdown to free things so that we run cleanly
 * under valgrind */
void cfg_shutdown(void)
{
  if (global_cfg) {
    json_decref(global_cfg);
  }
  if (arg_cfg) {
    json_decref(arg_cfg);
  }
}

void cfg_load_global_config_file(void)
{
  json_t *config = NULL;
  json_error_t err;

  const char *cfg_file = getenv("WATCHMAN_CONFIG_FILE");
#ifdef WATCHMAN_CONFIG_FILE
  if (!cfg_file) {
    cfg_file = WATCHMAN_CONFIG_FILE;
  }
#endif
  if (!cfg_file || cfg_file[0] == '\0') {
    return;
  }

  if (access(cfg_file, R_OK) != 0 && errno == ENOENT) {
    return;
  }

  config = json_load_file(cfg_file, 0, &err);
  if (!config) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
          cfg_file, err.text);
    return;
  }

  global_cfg = config;
}

void cfg_set_arg(const char *name, json_t *val)
{
  pthread_rwlock_wrlock(&cfg_lock);
  if (!arg_cfg) {
    arg_cfg = json_object();
  }

  json_object_set_nocheck(arg_cfg, name, val);

  pthread_rwlock_unlock(&cfg_lock);
}

void cfg_set_global(const char *name, json_t *val)
{
  pthread_rwlock_wrlock(&cfg_lock);
  if (!global_cfg) {
    global_cfg = json_object();
  }

  json_object_set_nocheck(global_cfg, name, val);

  pthread_rwlock_unlock(&cfg_lock);
}

static json_t *cfg_get_raw(const char *name, json_t **optr)
{
  json_t *val = NULL;

  pthread_rwlock_rdlock(&cfg_lock);
  if (*optr) {
    val = json_object_get(*optr, name);
  }
  pthread_rwlock_unlock(&cfg_lock);
  return val;
}

static json_t *cfg_get_root(w_root_t *root, const char *name)
{
  json_t *val = NULL;

  // Highest precedence: options set on the root
  if (root && root->config_file) {
    val = json_object_get(root->config_file, name);
  }
  // then: command line arguments
  if (!val) {
    val = cfg_get_raw(name, &arg_cfg);
  }
  // then: global config options
  if (!val) {
    val = cfg_get_raw(name, &global_cfg);
  }
  return val;
}

const char *cfg_get_string(w_root_t *root, const char *name,
    const char *defval)
{
  json_t *val = cfg_get_root(root, name);

  if (val) {
    if (!json_is_string(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a string\n", name);
    }
    return json_string_value(val);
  }

  return defval;
}

json_int_t cfg_get_int(w_root_t *root, const char *name,
    json_int_t defval)
{
  json_t *val = cfg_get_root(root, name);

  if (val) {
    if (!json_is_integer(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an integer\n", name);
    }
    return json_integer_value(val);
  }

  return defval;
}

/* vim:ts=2:sw=2:et:
 */

