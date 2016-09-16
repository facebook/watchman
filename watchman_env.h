/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

w_ht_t* w_envp_make_ht(void);
char** w_envp_make_from_ht(w_ht_t* ht, uint32_t* env_size);
void w_envp_set_cstring(w_ht_t* envht, const char* key, const char* val);
void w_envp_set(w_ht_t* envht, const char* key, w_string_t* val);
void w_envp_set_list(w_ht_t* envht, const char* key, json_t* arr);
void w_envp_set_bool(w_ht_t* envht, const char* key, bool val);
void w_envp_unset(w_ht_t* envht, const char* key);
