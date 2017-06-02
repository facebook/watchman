/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

void cfg_shutdown(void);
void cfg_set_arg(const char* name, const json_ref& val);
void cfg_load_global_config_file(void);
json_ref cfg_get_json(const char* name);
const char* cfg_get_string(const char* name, const char* defval);
json_int_t cfg_get_int(const char* name, json_int_t defval);
bool cfg_get_bool(const char* name, bool defval);
double cfg_get_double(const char* name, double defval);
#ifndef _WIN32
mode_t cfg_get_perms(const char* name, bool write_bits, bool execute_bits);
#endif
const char *cfg_get_trouble_url(void);
json_ref cfg_compute_root_files(bool* enforcing);

class Configuration {
  json_ref local_;

 public:
  Configuration() = default;
  explicit Configuration(const json_ref& local);

  json_ref get(const char* name) const;
  const char* getString(const char* name, const char* defval) const;
  json_int_t getInt(const char* name, json_int_t defval) const;
  bool getBool(const char* name, bool defval) const;
  double getDouble(const char* name, double defval) const;
};
