/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

std::unordered_map<w_string, w_string> w_envp_make_ht(void);
char** w_envp_make_from_ht(
    const std::unordered_map<w_string, w_string>& ht,
    uint32_t* env_size);
void w_envp_set_cstring(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    const char* val);
void w_envp_set(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    w_string_t* val);
void w_envp_set_bool(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    bool val);
void w_envp_unset(
    std::unordered_map<w_string, w_string>& envht,
    const char* key);
