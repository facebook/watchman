/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_synchronized.h"

namespace {
struct config_state {
  json_ref global_cfg;
  json_ref arg_cfg;
};
watchman::Synchronized<config_state> configState;
}

/* Called during shutdown to free things so that we run cleanly
 * under valgrind */
void cfg_shutdown(void)
{
  auto state = configState.wlock();
  state->global_cfg.reset();
  state->arg_cfg.reset();
}

void cfg_load_global_config_file(void)
{
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

  if (!w_path_exists(cfg_file)) {
    return;
  }

  auto config = json_load_file(cfg_file, 0, &err);
  if (!config) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
          cfg_file, err.text);
    return;
  }

  configState.wlock()->global_cfg = config;
}

void cfg_set_arg(const char* name, const json_ref& val) {
  auto state = configState.wlock();
  if (!state->arg_cfg) {
    state->arg_cfg = json_object();
  }

  state->arg_cfg.set(name, json_ref(val));
}

void cfg_set_global(const char* name, const json_ref& val) {
  auto state = configState.wlock();
  if (!state->global_cfg) {
    state->global_cfg = json_object();
  }

  state->global_cfg.set(name, json_ref(val));
}

static json_ref cfg_get_raw(const char* name, const json_ref* optr) {
  json_ref val;

  if (*optr) {
    val = optr->get_default(name);
  }
  return val;
}

json_ref cfg_get_json(const char* name) {
  json_ref val;
  auto state = configState.rlock();

  // Highest precedence: command line arguments
  val = cfg_get_raw(name, &state->arg_cfg);
  // then: global config options
  if (!val) {
    val = cfg_get_raw(name, &state->global_cfg);
  }
  return val;
}

const char* cfg_get_string(const char* name, const char* defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!json_is_string(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a string\n", name);
    }
    return json_string_value(val);
  }

  return defval;
}

// Return true if the json ref is an array of string values
static bool is_array_of_strings(const json_ref& ref) {
  uint32_t i;

  if (!json_is_array(ref)) {
    return false;
  }

  for (i = 0; i < json_array_size(ref); i++) {
    if (!json_is_string(json_array_get(ref, i))) {
      return false;
    }
  }
  return true;
}

// Given an array of string values, if that array does not contain
// a ".watchmanconfig" entry, prepend it
static void prepend_watchmanconfig_to_array(json_ref& ref) {
  const char *val;

  if (json_array_size(ref) == 0) {
    // json_array_insert_new at index can fail when the array is empty,
    // so just append in this case.
    json_array_append_new(ref, typed_string_to_json(".watchmanconfig",
          W_STRING_UNICODE));
    return;
  }

  val = json_string_value(json_array_get(ref, 0));
  if (!strcmp(val, ".watchmanconfig")) {
    return;
  }
  json_array_insert_new(ref, 0, typed_string_to_json(".watchmanconfig",
        W_STRING_UNICODE));
}

// Compute the effective value of the root_files configuration and
// return a json reference.  The caller must decref the ref when done
// (we may synthesize this value).   Sets enforcing to indicate whether
// we will only allow watches on the root_files.
// The array returned by this function (if not NULL) is guaranteed to
// list .watchmanconfig as its zeroth element.
json_ref cfg_compute_root_files(bool* enforcing) {
  *enforcing = false;

  json_ref ref = cfg_get_json("enforce_root_files");
  if (ref) {
    if (!json_is_boolean(ref)) {
      w_log(W_LOG_FATAL,
          "Expected config value enforce_root_files to be boolean\n");
    }
    *enforcing = json_is_true(ref);
  }

  ref = cfg_get_json("root_files");
  if (ref) {
    if (!is_array_of_strings(ref)) {
      w_log(W_LOG_FATAL,
          "global config root_files must be an array of strings\n");
      *enforcing = false;
      return nullptr;
    }
    prepend_watchmanconfig_to_array(ref);

    return ref;
  }

  // Try legacy root_restrict_files configuration
  ref = cfg_get_json("root_restrict_files");
  if (ref) {
    if (!is_array_of_strings(ref)) {
      w_log(W_LOG_FATAL, "deprecated global config root_restrict_files "
          "must be an array of strings\n");
      *enforcing = false;
      return nullptr;
    }
    prepend_watchmanconfig_to_array(ref);
    *enforcing = true;
    return ref;
  }

  // Synthesize our conservative default value.
  // .watchmanconfig MUST be first
  return json_array({typed_string_to_json(".watchmanconfig"),
                     typed_string_to_json(".hg"),
                     typed_string_to_json(".git"),
                     typed_string_to_json(".svn")});
}

json_int_t cfg_get_int(const char* name, json_int_t defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!json_is_integer(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an integer\n", name);
    }
    return json_integer_value(val);
  }

  return defval;
}

bool cfg_get_bool(const char* name, bool defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!json_is_boolean(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a boolean\n", name);
    }
    return json_is_true(val);
  }

  return defval;
}

double cfg_get_double(const char* name, double defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!json_is_number(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a number\n", name);
    }
    return json_real_value(val);
  }

  return defval;
}

#ifndef _WIN32
#define MAKE_GET_PERM(PROP, SUFFIX)                                 \
  static mode_t get_##PROP##_perm(                                  \
      const char* name,                                             \
      const json_ref& val,                                          \
      bool write_bits,                                              \
      bool execute_bits) {                                          \
    mode_t ret = 0;                                                 \
    auto perm = val.get_default(#PROP);                             \
    if (perm) {                                                     \
      if (!json_is_boolean(perm)) {                                 \
        w_log(                                                      \
            W_LOG_FATAL,                                            \
            "Expected config value %s." #PROP " to be a boolean\n", \
            name);                                                  \
      }                                                             \
      if (json_is_true(perm)) {                                     \
        ret |= S_IR##SUFFIX;                                        \
        if (write_bits) {                                           \
          ret |= S_IW##SUFFIX;                                      \
        }                                                           \
        if (execute_bits) {                                         \
          ret |= S_IX##SUFFIX;                                      \
        }                                                           \
      }                                                             \
    }                                                               \
    return ret;                                                     \
  }

MAKE_GET_PERM(group, GRP)
MAKE_GET_PERM(others, OTH)

/**
 * This function expects the config to be an object containing the keys 'group'
 * and 'others', each a bool.
 */
mode_t cfg_get_perms(const char* name, bool write_bits, bool execute_bits) {
  auto val = cfg_get_json(name);
  mode_t ret = S_IRUSR | S_IWUSR;
  if (execute_bits) {
    ret |= S_IXUSR;
  }

  if (val) {
    if (!json_is_object(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an object\n", name);
    }

    ret |= get_group_perm(name, val, write_bits, execute_bits);
    ret |= get_others_perm(name, val, write_bits, execute_bits);
  }

  return ret;
}
#endif

const char *cfg_get_trouble_url(void) {
  return cfg_get_string(
      "troubleshooting_url",
      "https://facebook.github.io/watchman/docs/troubleshooting.html");
}

Configuration::Configuration(const json_ref& local) : local_(local) {}

json_ref Configuration::get(const char* name) const {
  // Highest precedence: options set locally
  json_ref val;
  if (local_) {
    val = local_.get_default(name);
    if (val) {
      return val;
    }
  }
  auto state = configState.rlock();

  // then: command line arguments
  if (!val) {
    val = cfg_get_raw(name, &state->arg_cfg);
  }
  // then: global config options
  if (!val) {
    val = cfg_get_raw(name, &state->global_cfg);
  }
  return val;
}

const char* Configuration::getString(const char* name, const char* defval)
    const {
  auto val = get(name);

  if (val) {
    if (!json_is_string(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a string\n", name);
    }
    return json_string_value(val);
  }

  return defval;
}

json_int_t Configuration::getInt(const char* name, json_int_t defval) const {
  auto val = get(name);

  if (val) {
    if (!json_is_integer(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be an integer\n", name);
    }
    return json_integer_value(val);
  }

  return defval;
}

bool Configuration::getBool(const char* name, bool defval) const {
  auto val = get(name);

  if (val) {
    if (!json_is_boolean(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a boolean\n", name);
    }
    return json_is_true(val);
  }

  return defval;
}

double Configuration::getDouble(const char* name, double defval) const {
  auto val = get(name);

  if (val) {
    if (!json_is_number(val)) {
      w_log(W_LOG_FATAL, "Expected config value %s to be a number\n", name);
    }
    return json_real_value(val);
  }

  return defval;
}

/* vim:ts=2:sw=2:et:
 */
