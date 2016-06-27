/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Constructs a hash table from the current process environment */
w_ht_t *w_envp_make_ht(void)
{
  w_ht_t *ht;
  uint32_t nenv, i;
  const char *eq;
  const char *ent;
  w_string_t *key, *val, *str;

  for (i = 0, nenv = 0; environ[i]; i++) {
    nenv++;
  }

  ht = w_ht_new(nenv, &w_ht_dict_funcs);

  for (i = 0; environ[i]; i++) {
    ent = environ[i];
    eq = strchr(ent, '=');
    if (!eq) {
      continue;
    }

    // slice name=value into a key and a value string
    str = w_string_new(ent);
    key = w_string_slice(str, 0, (uint32_t)(eq - ent));
    val = w_string_slice(str, 1 + (uint32_t)(eq - ent),
            (uint32_t)(str->len - (key->len + 1)));

    // Replace rather than set, just in case we somehow have duplicate
    // keys in our environment array.
    w_ht_replace(ht, w_ht_ptr_val(key), w_ht_ptr_val(val));

    // Release our slices
    w_string_delref(str);
    w_string_delref(key);
    w_string_delref(val);
  }

  return ht;
}

/* Constructs an envp array from a hash table.
 * The returned array occupies a single contiguous block of memory
 * such that it can be released by a single call to free(3).
 * The last element of the returned array is set to NULL for compatibility
 * with posix_spawn() */
char **w_envp_make_from_ht(w_ht_t *ht, uint32_t *env_size)
{
  auto nele = w_ht_size(ht);
  auto len = (1 + nele) * sizeof(char*);
  w_ht_iter_t iter;
  char **envp;
  char *buf;
  uint32_t i = 0;

  // Make a pass through to compute the required memory size
  if (w_ht_first(ht, &iter)) do {
    w_string_t *key = (w_string_t*)w_ht_val_ptr(iter.key);
    w_string_t *val = (w_string_t*)w_ht_val_ptr(iter.value);

    // key=value\0
    len += key->len + 1 + val->len + 1;
  } while (w_ht_next(ht, &iter));

  *env_size = (uint32_t)len;

  envp = (char**)malloc(len);
  if (!envp) {
    return NULL;
  }

  buf = (char*)(envp + nele + 1);

  // Now populate
  if (w_ht_first(ht, &iter)) do {
    w_string_t *key = (w_string_t*)w_ht_val_ptr(iter.key);
    w_string_t *val = (w_string_t*)w_ht_val_ptr(iter.value);

    envp[i++] = buf;

    // key=value\0
    memcpy(buf, key->buf, key->len);
    buf += key->len;

    memcpy(buf, "=", 1);
    buf++;

    memcpy(buf, val->buf, val->len);
    buf += val->len;

    *buf = 0;
    buf++;
  } while (w_ht_next(ht, &iter));

  envp[nele] = NULL;

  return envp;
}

void w_envp_set_bool(w_ht_t *envht, const char *key, bool val)
{
  if (val) {
    w_envp_set_cstring(envht, key, "true");
  } else {
    w_envp_unset(envht, key);
  }
}

void w_envp_unset(w_ht_t *envht, const char *key)
{
  w_string_t *kstr = w_string_new(key);

  w_ht_del(envht, w_ht_ptr_val(kstr));

  w_string_delref(kstr);
}

void w_envp_set(w_ht_t *envht, const char *key, w_string_t *val)
{
  w_string_t *kstr = w_string_new(key);

  w_ht_replace(envht, w_ht_ptr_val(kstr), w_ht_ptr_val(val));

  w_string_delref(kstr);
}

void w_envp_set_cstring(w_ht_t *envht, const char *key, const char *val)
{
  w_string_t *kstr = w_string_new(key);
  w_string_t *vstr = w_string_new(val);

  w_ht_replace(envht, w_ht_ptr_val(kstr), w_ht_ptr_val(vstr));

  w_string_delref(kstr);
  w_string_delref(vstr);
}

/* vim:ts=2:sw=2:et:
 */
