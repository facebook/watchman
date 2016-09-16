/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

void cfg_shutdown(void);
void cfg_set_arg(const char* name, json_t* val);
void cfg_load_global_config_file(void);
json_t* cfg_get_json(const w_root_t* root, const char* name);
const char*
cfg_get_string(const w_root_t* root, const char* name, const char* defval);
json_int_t
cfg_get_int(const w_root_t* root, const char* name, json_int_t defval);
bool cfg_get_bool(const w_root_t* root, const char* name, bool defval);
double cfg_get_double(const w_root_t* root, const char* name, double defval);
mode_t cfg_get_perms(
    const w_root_t* root,
    const char* name,
    bool write_bits,
    bool execute_bits);
const char *cfg_get_trouble_url(void);
json_t *cfg_compute_root_files(bool *enforcing);
