/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Re-implementing hash tables again :-/

struct watchman_hash_bucket {
  struct watchman_hash_bucket *next, *prev;
  w_ht_val_t key, value;
};

struct watchman_hash_table {
  uint32_t nelems;
  uint32_t table_size;
  struct watchman_hash_bucket **table;
  const struct watchman_hash_funcs *funcs;
};

w_ht_t *w_ht_new(uint32_t size_hint, const struct watchman_hash_funcs *funcs)
{
  w_ht_t *ht = calloc(1, sizeof(*ht));

  if (!ht) {
    return NULL;
  }

  ht->table_size = next_power_2(size_hint);
  ht->table = calloc(ht->table_size, sizeof(void*));
  if (!ht->table) {
    free(ht);
    return NULL;
  }

  ht->funcs = funcs;
  return ht;
}

void w_ht_free(w_ht_t *ht)
{
  struct watchman_hash_bucket *b;
  uint32_t slot;

  for (slot = 0; slot < ht->table_size; slot++) {
    while (ht->table[slot]) {
      b = ht->table[slot];
      ht->table[slot] = b->next;

      if (ht->funcs && ht->funcs->del_val) {
        ht->funcs->del_val(b->value);
      }
      if (ht->funcs && ht->funcs->del_key) {
        ht->funcs->del_key(b->key);
      }
      free(b);
    }
  }
  free(ht->table);
  free(ht);
}

static inline uint32_t compute_hash(w_ht_t *ht, w_ht_val_t key)
{
  if (ht->funcs && ht->funcs->hash_key) {
    return ht->funcs->hash_key(key);
  }
  return (uint32_t)key;
}

static inline bool equal_key(w_ht_t *ht, w_ht_val_t a, w_ht_val_t b)
{
  if (ht->funcs && ht->funcs->equal_key) {
    return ht->funcs->equal_key(a, b);
  }
  return a == b;
}

static void resize(w_ht_t *ht, uint32_t newsize)
{
  struct watchman_hash_bucket **table;
  uint32_t slot;

  table = calloc(newsize, sizeof(void*));
  if (!table) {
    return;
  }

  // Don't log from in here, as we may be called with
  // the client lock held, and attempting to lock in
  // here will deadlock with ourselves!

  for (slot = 0; slot < ht->table_size; slot++) {
    while (ht->table[slot]) {
      struct watchman_hash_bucket *b = ht->table[slot];
      uint32_t nslot;

      ht->table[slot] = b->next;

      nslot = compute_hash(ht, b->key) & (newsize - 1);
      b->prev = NULL;
      b->next = table[nslot];
      if (b->next) {
        b->next->prev = b;
      }
      table[nslot] = b;
    }
  }
  free(ht->table);
  ht->table = table;
  ht->table_size = newsize;
}

bool w_ht_set(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value)
{
  return w_ht_insert(ht, key, value, false);
}

bool w_ht_replace(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value)
{
  return w_ht_insert(ht, key, value, true);
}

/* Compute the ideal table size.
 * Hash table literature suggests that the ideal load factor for a
 * table is approximately 0.7.
 * Our ideal size is therefore a bit larger, and rounded up to
 * a power of 2 */
static inline uint32_t ideal_size(w_ht_t *ht)
{
  return next_power_2(ht->nelems * 3 / 2);
}

bool w_ht_insert(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value, bool replace)
{
  struct watchman_hash_bucket *b;
  uint32_t slot = compute_hash(ht, key) & (ht->table_size - 1);
  uint32_t ideal;

  for (b = ht->table[slot]; b; b = b->next) {
    if (equal_key(ht, key, b->key)) {
      if (!replace) {
        errno = EEXIST;
        return false;
      }

      /* copy the value before we delete the old one, in case
       * the values somehow reference the same thing; we don't
       * want to delete it from under ourselves */
      if (ht->funcs && ht->funcs->copy_val) {
        value = ht->funcs->copy_val(value);
      }
      if (ht->funcs && ht->funcs->del_val) {
        ht->funcs->del_val(b->value);
      }
      b->value = value;

      return true;
    }
  }

  b = malloc(sizeof(*b));
  if (!b) {
    errno = ENOMEM;
    return false;
  }

  if (ht->funcs && ht->funcs->copy_key) {
    key = ht->funcs->copy_key(key);
  }
  if (ht->funcs && ht->funcs->copy_val) {
    value = ht->funcs->copy_val(value);
  }
  b->key = key;
  b->value = value;
  b->prev = NULL;
  b->next = ht->table[slot];
  if (b->next) {
    b->next->prev = b;
  }
  ht->table[slot] = b;
  ht->nelems++;

  ideal = ideal_size(ht);
  if (ht->nelems > ideal) {
    resize(ht, ideal);
  }

  return true;
}

w_ht_val_t w_ht_get(w_ht_t *ht, w_ht_val_t key)
{
  w_ht_val_t val = 0;

  w_ht_lookup(ht, key, &val, false);
  return val;
}

bool w_ht_lookup(w_ht_t *ht, w_ht_val_t key, w_ht_val_t *val, bool copy)
{
  struct watchman_hash_bucket *b;
  uint32_t slot = compute_hash(ht, key) & (ht->table_size - 1);

  for (b = ht->table[slot]; b; b = b->next) {
    if (equal_key(ht, key, b->key)) {
      if (copy && ht->funcs && ht->funcs->copy_val) {
        *val = ht->funcs->copy_val(b->value);
      } else {
        *val = b->value;
      }
      return true;
    }
  }

  return false;
}

static inline void delete_bucket(w_ht_t *ht,
    struct watchman_hash_bucket *b, int slot,
    bool do_resize)
{
  if (b->next) {
    b->next->prev = b->prev;
  }
  if (b->prev) {
    b->prev->next = b->next;
  }
  if (b == ht->table[slot]) {
    ht->table[slot] = b->next;
  }
  if (ht->funcs && ht->funcs->del_key) {
    ht->funcs->del_key(b->key);
  }
  if (ht->funcs && ht->funcs->del_val) {
    ht->funcs->del_val(b->value);
  }
  free(b);
  ht->nelems--;

  if (do_resize) {
    uint32_t shrink = ideal_size(ht);

    if (ht->table_size > shrink) {
      resize(ht, shrink);
    }
  }
}

static bool perform_delete(w_ht_t *ht, w_ht_val_t key, bool do_resize)
{
  struct watchman_hash_bucket *b;
  uint32_t slot = compute_hash(ht, key) & (ht->table_size - 1);

  for (b = ht->table[slot]; b; b = b->next) {
    if (equal_key(ht, key, b->key)) {
      delete_bucket(ht, b, slot, do_resize);
      return true;
    }
  }
  return false;
}

bool w_ht_del(w_ht_t *ht, w_ht_val_t key)
{
  return perform_delete(ht, key, true);
}

uint32_t w_ht_size(w_ht_t *ht)
{
  return ht->nelems;
}

uint32_t w_ht_num_buckets(w_ht_t *ht)
{
  return ht->table_size;
}

bool w_ht_first(w_ht_t *ht, w_ht_iter_t *iter)
{
  if (!ht) return false;
  if (!ht->nelems) return false;

  iter->slot = -1;
  iter->ptr = NULL;

  return w_ht_next(ht, iter);
}

bool w_ht_next(w_ht_t *ht, w_ht_iter_t *iter)
{
  struct watchman_hash_bucket *b = iter->ptr;

  if (iter->slot != (uint32_t)-1 && iter->slot >= ht->table_size) {
    return false;
  }

  if (b && b->next) {
    iter->ptr = b->next;
  } else {
    do {
      iter->slot++;
      if (iter->slot >= ht->table_size) {
        return false;
      }
      iter->ptr = ht->table[iter->slot];
    } while (!iter->ptr);
  }

  if (iter->ptr) {
    b = iter->ptr;
    iter->key = b->key;
    iter->value = b->value;

    return true;
  }

  return false;
}

/* iterator aware delete */
bool w_ht_iter_del(w_ht_t *ht, w_ht_iter_t *iter)
{
  struct watchman_hash_bucket *b;

  b = iter->ptr;
  if (!iter->ptr) {
    return false;
  }

  /* walk back to the prior iterm, because ht_next will be used
   * to walk forwards; we want to land on the next item and not skip
   * it */
  if (b->prev) {
    iter->ptr = b->prev;
  } else {
    /* we were the front of that bucket slot, arrange for iteration
     * to find the front of it again */
    iter->ptr = NULL;
  }

  delete_bucket(ht, b, iter->slot, false);

  return true;
}

w_ht_val_t w_ht_string_copy(w_ht_val_t key)
{
  w_string_addref((w_string_t*)key);
  return key;
}

void w_ht_string_del(w_ht_val_t key)
{
  w_string_delref((w_string_t*)key);
}

bool w_ht_string_equal(w_ht_val_t a, w_ht_val_t b)
{
  return w_string_equal((w_string_t*)a, (w_string_t*)b);
}

uint32_t w_ht_string_hash(w_ht_val_t key)
{
  return ((w_string_t*)key)->hval;
}

const struct watchman_hash_funcs w_ht_string_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  NULL
};

/* vim:ts=2:sw=2:et:
 */
