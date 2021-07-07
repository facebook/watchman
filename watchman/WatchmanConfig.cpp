/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/WatchmanConfig.h"
#include <folly/ExceptionString.h>
#include <folly/Synchronized.h>
#include <optional>
#include "watchman/Errors.h"
#include "watchman/Logging.h"

using namespace watchman;

namespace {

struct ConfigState {
  json_ref global_cfg;
  w_string global_config_file_path;
  json_ref arg_cfg;
};
folly::Synchronized<ConfigState> configState;

std::optional<std::pair<json_ref, w_string>> loadSystemConfig() {
  const char* cfg_file = getenv("WATCHMAN_CONFIG_FILE");
#ifdef WATCHMAN_CONFIG_FILE
  if (!cfg_file) {
    cfg_file = WATCHMAN_CONFIG_FILE;
  }
#endif
  if (!cfg_file || cfg_file[0] == '\0') {
    return std::nullopt;
  }

  std::string cfg_file_default = std::string{cfg_file} + ".default";
  const char* current_cfg_file;

  json_ref config;
  try {
    // Try to load system watchman configuration
    try {
      current_cfg_file = cfg_file;
      config = json_load_file(current_cfg_file, 0);
    } catch (const std::system_error& exc) {
      if (exc.code() == watchman::error_code::no_such_file_or_directory) {
        // Fallback to trying to load default watchman configuration if there
        // is no system configuration
        try {
          current_cfg_file = cfg_file_default.c_str();
          config = json_load_file(current_cfg_file, 0);
        } catch (const std::system_error& default_exc) {
          // If there is no default configuration either, just return
          if (default_exc.code() ==
              watchman::error_code::no_such_file_or_directory) {
            return std::nullopt;
          } else {
            throw;
          }
        }
      } else {
        throw;
      }
    }
  } catch (const std::system_error& exc) {
    logf(
        ERR,
        "Failed to load config file {}: {}\n",
        current_cfg_file,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  } catch (const std::exception& exc) {
    logf(
        ERR,
        "Failed to parse config file {}: {}\n",
        current_cfg_file,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  }

  if (!config.isObject()) {
    logf(ERR, "config {} must be a JSON object\n", current_cfg_file);
    return std::nullopt;
  }

  return {{config, current_cfg_file}};
}

std::optional<json_ref> loadUserConfig() {
  const char* home = getenv("HOME");
  if (!home) {
    return std::nullopt;
  }
  auto path = std::string{home} + "/.watchman.json";
  try {
    json_ref config = json_load_file(path.c_str(), 0);
    if (!config.isObject()) {
      logf(ERR, "config {} must be a JSON object\n", path);
      return std::nullopt;
    }
    return config;
  } catch (const std::system_error& exc) {
    if (exc.code() == watchman::error_code::no_such_file_or_directory) {
      return std::nullopt;
    }
    logf(
        ERR,
        "Failed to load config file {}: {}\n",
        path,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  } catch (const std::exception& exc) {
    logf(
        ERR,
        "Failed to parse config file {}: {}\n",
        path,
        folly::exceptionStr(exc).toStdString());
    return std::nullopt;
  }
}

} // namespace

/* Called during shutdown to free things so that we run cleanly
 * under valgrind */
void cfg_shutdown() {
  auto state = configState.wlock();
  state->global_cfg.reset();
  state->arg_cfg.reset();
}

w_string cfg_get_global_config_file_path() {
  return configState.rlock()->global_config_file_path;
}

void cfg_load_global_config_file() {
  auto systemConfig = loadSystemConfig();
  auto userConfig = loadUserConfig();

  auto lockedState = configState.wlock();
  if (systemConfig) {
    lockedState->global_cfg = systemConfig->first;
    lockedState->global_config_file_path = systemConfig->second;
  }

  if (userConfig) {
    if (!lockedState->global_cfg) {
      lockedState->global_cfg = json_object();
    }
    for (auto& [key, value] : userConfig->object()) {
      lockedState->global_cfg.object()[key] = value;
    }
  }
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
    if (!val.isString()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a string"));
    }
    return json_string_value(val);
  }

  return defval;
}

// Return true if the json ref is an array of string values
static bool is_array_of_strings(const json_ref& ref) {
  uint32_t i;

  if (!ref.isArray()) {
    return false;
  }

  for (i = 0; i < json_array_size(ref); i++) {
    if (!json_array_get(ref, i).isString()) {
      return false;
    }
  }
  return true;
}

// Given an array of string values, if that array does not contain
// a ".watchmanconfig" entry, prepend it
static void prepend_watchmanconfig_to_array(json_ref& ref) {
  const char* val;

  if (json_array_size(ref) == 0) {
    // json_array_insert_new at index can fail when the array is empty,
    // so just append in this case.
    json_array_append_new(
        ref, typed_string_to_json(".watchmanconfig", W_STRING_UNICODE));
    return;
  }

  val = json_string_value(json_array_get(ref, 0));
  if (!strcmp(val, ".watchmanconfig")) {
    return;
  }
  json_array_insert_new(
      ref, 0, typed_string_to_json(".watchmanconfig", W_STRING_UNICODE));
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
    if (!ref.isBool()) {
      logf(FATAL, "Expected config value enforce_root_files to be boolean\n");
    }
    *enforcing = ref.asBool();
  }

  ref = cfg_get_json("root_files");
  if (ref) {
    if (!is_array_of_strings(ref)) {
      logf(FATAL, "global config root_files must be an array of strings\n");
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
      logf(
          FATAL,
          "deprecated global config root_restrict_files "
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
  return json_array(
      {typed_string_to_json(".watchmanconfig"),
       typed_string_to_json(".hg"),
       typed_string_to_json(".git"),
       typed_string_to_json(".svn")});
}

// Produces a string like:  "`foo`, `bar`, and `baz`"
std::string cfg_pretty_print_root_files(const json_ref& root_files) {
  std::string result;
  for (unsigned int i = 0; i < root_files.array().size(); ++i) {
    const auto& r = root_files.array()[i];
    if (i > 1 && i == root_files.array().size() - 1) {
      // We are last in a list of multiple items
      result.append(", and ");
    } else if (i > 0) {
      result.append(", ");
    }
    result.append("`");
    result.append(json_string_value(r));
    result.append("`");
  }
  return result;
}

json_int_t cfg_get_int(const char* name, json_int_t defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!val.isInt()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be an integer"));
    }
    return val.asInt();
  }

  return defval;
}

bool cfg_get_bool(const char* name, bool defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!val.isBool()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a boolean"));
    }
    return val.asBool();
  }

  return defval;
}

double cfg_get_double(const char* name, double defval) {
  auto val = cfg_get_json(name);

  if (val) {
    if (!val.isNumber()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a number"));
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
      if (!perm.isBool()) {                                         \
        logf(                                                       \
            FATAL,                                                  \
            "Expected config value {}." #PROP " to be a boolean\n", \
            name);                                                  \
      }                                                             \
      if (perm.asBool()) {                                          \
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
    if (!val.isObject()) {
      logf(FATAL, "Expected config value {} to be an object\n", name);
    }

    ret |= get_group_perm(name, val, write_bits, execute_bits);
    ret |= get_others_perm(name, val, write_bits, execute_bits);
  }

  return ret;
}
#endif

const char* cfg_get_trouble_url() {
  return cfg_get_string(
      "troubleshooting_url",
      "https://facebook.github.io/watchman/docs/troubleshooting.html");
}

namespace watchman {

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
    if (!val.isString()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a string"));
    }
    return json_string_value(val);
  }

  return defval;
}

json_int_t Configuration::getInt(const char* name, json_int_t defval) const {
  auto val = get(name);

  if (val) {
    if (!val.isInt()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be an integer"));
    }
    return val.asInt();
  }

  return defval;
}

bool Configuration::getBool(const char* name, bool defval) const {
  auto val = get(name);

  if (val) {
    if (!val.isBool()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a boolean"));
    }
    return val.asBool();
  }

  return defval;
}

double Configuration::getDouble(const char* name, double defval) const {
  auto val = get(name);

  if (val) {
    if (!val.isNumber()) {
      throw std::runtime_error(folly::to<std::string>(
          "Expected config value ", name, " to be a number"));
    }
    return json_real_value(val);
  }

  return defval;
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
