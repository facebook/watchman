/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Constructs a hash table from the current process environment */
std::unordered_map<w_string, w_string> w_envp_make_ht(void) {
  std::unordered_map<w_string, w_string> ht;
  uint32_t nenv, i;
  const char *eq;
  const char *ent;

  for (i = 0, nenv = 0; environ[i]; i++) {
    nenv++;
  }

  ht.reserve(nenv);

  for (i = 0; environ[i]; i++) {
    ent = environ[i];
    eq = strchr(ent, '=');
    if (!eq) {
      continue;
    }

    // slice name=value into a key and a value string
    w_string str(ent, W_STRING_BYTE);
    auto key = str.slice(0, (uint32_t)(eq - ent));
    auto val = str.slice(
        1 + (uint32_t)(eq - ent), (uint32_t)(str.size() - (key.size() + 1)));

    // Replace rather than set, just in case we somehow have duplicate
    // keys in our environment array.
    ht[key] = val;
  }

  return ht;
}

/* Constructs an envp array from a hash table.
 * The returned array occupies a single contiguous block of memory
 * such that it can be released by a single call to free(3).
 * The last element of the returned array is set to NULL for compatibility
 * with posix_spawn() */
char** w_envp_make_from_ht(
    const std::unordered_map<w_string, w_string>& ht,
    uint32_t* env_size) {
  int nele = ht.size();
  int len = (1 + nele) * sizeof(char*);
  char *buf;
  uint32_t i = 0;

  // Make a pass through to compute the required memory size
  for (const auto& it : ht) {
    const auto& key = it.first;
    const auto& val = it.second;

    // key=value\0
    len += key.size() + 1 + val.size() + 1;
  }

  *env_size = len;

  auto envp = (char**)malloc(len);
  if (!envp) {
    return NULL;
  }

  buf = (char*)(envp + nele + 1);

  // Now populate
  for (const auto& it : ht) {
    const auto& key = it.first;
    const auto& val = it.second;

    envp[i++] = buf;

    // key=value\0
    memcpy(buf, key.data(), key.size());
    buf += key.size();

    memcpy(buf, "=", 1);
    buf++;

    memcpy(buf, val.data(), val.size());
    buf += val.size();

    *buf = 0;
    buf++;
  }

  envp[nele] = NULL;

  return envp;
}

void w_envp_set_bool(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    bool val) {
  if (val) {
    w_envp_set_cstring(envht, key, "true");
  } else {
    w_envp_unset(envht, key);
  }
}

void w_envp_unset(
    std::unordered_map<w_string, w_string>& envht,
    const char* key) {
  envht.erase(key);
}

void w_envp_set(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    w_string_t* val) {
  envht[key] = val;
}

void w_envp_set_cstring(
    std::unordered_map<w_string, w_string>& envht,
    const char* key,
    const char* val) {
  envht[key] = val;
}

/* vim:ts=2:sw=2:et:
 */
