/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WATCHMAN_HASH_H
#define WATCHMAN_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_hash_table;
typedef struct watchman_hash_table w_ht_t;

/* hash table key/value data type, large enough to hold a pointer
 * or a 64-bit value */
typedef int64_t w_ht_val_t;

/* copies a key.  If NULL, simply does a bit copy, but you
 * can provide an implementation that manages a refcount */
typedef w_ht_val_t (*w_hash_table_copy_t)(w_ht_val_t key);

/* deletes a key.  If NULL, simply NOPs, but you can
 * provide an implementation that manages a refcount */
typedef void (*w_hash_table_delete_t)(w_ht_val_t key);

/* compares a key for equality.  If NULL, simply does
 * a bit compare */
typedef bool (*w_hash_table_equal_t)(w_ht_val_t a, w_ht_val_t b);

/* computes the hash value of a key.  If NULL, simply
 * takes the bit value truncated to 32-bits */
typedef uint32_t (*w_hash_table_hash_t)(w_ht_val_t key);

struct watchman_hash_funcs {
  w_hash_table_copy_t   copy_key;
  w_hash_table_delete_t del_key;
  w_hash_table_equal_t  equal_key;
  w_hash_table_hash_t   hash_key;
  w_hash_table_copy_t   copy_val;
  w_hash_table_delete_t del_val;
};

/* create a new hash table.
 * size_hint is used to pre-allocate the number of buckets
 * and is useful to reduce realloc() traffic if you know
 * how big the table is going to be at creation time. */
w_ht_t *w_ht_new(uint32_t size_hint, const struct watchman_hash_funcs *funcs);

/* Destroy a hash table and free all associated resources */
void w_ht_free(w_ht_t *ht);

/* equivalent to calling w_ht_insert with replace=false */
bool w_ht_set(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value);

/* equivalent to calling w_ht_insert with replace=true */
bool w_ht_replace(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value);

/* set the value associated with the specified key.
 * If the element exists already, and replace==false,
 * returns false and leaves the table unmodified.
 *
 * If copy_key is defined, we'll invoke it on the key value IFF we
 * create a new element in the hash table.  If the element already
 * existed, we'll preserve the key from the prior insert.
 *
 * If we're replacing an existing value and del_val is defined,
 * we'll invoke it on the value that is already present in the table,
 *
 * If copy_val is defined, we'll invoke it on the value IFF we
 * create a new element in the hash table.
 */
bool w_ht_insert(w_ht_t *ht, w_ht_val_t key, w_ht_val_t value, bool replace);

/* Looks up the value associated with key and returns it.
 * Returns 0 if there was no matching value.
 *
 * If you need to distinguish between no value and the value 0,
 * use w_ht_lookup instead.
 *
 * This function will NOT invoke copy_val on the returned value.
 */
w_ht_val_t w_ht_get(w_ht_t *ht, w_ht_val_t key);

/* Looks up the value associated with key.
 * If found, stores the value into *VAL.
 * If copy==true and copy_val is defined, then it is invoked on the value
 * prior to storing it to *VAL.
 * Returns true if the value was found, false otherwise.
 */
bool w_ht_lookup(w_ht_t *ht, w_ht_val_t key, w_ht_val_t *val, bool copy);

/* Deletes the value associated with key.
 * If del_val is defined, it is invoked on the value stored in the table.
 * If del_key is defined, it is invoked on the key stored in the table.
 * Returns true if the element was present in the table, false otherwise.
 *
 * Do not call this function while iteration is in progress.
 * See w_ht_iter_del().
 */
bool w_ht_del(w_ht_t *ht, w_ht_val_t key);

/* Returns the number of elements stored in the table */
uint32_t w_ht_size(w_ht_t *ht);
/* Returns the number of buckets for diagnostic purposes */
uint32_t w_ht_num_buckets(w_ht_t *ht);

typedef struct {
  w_ht_val_t key;
  w_ht_val_t value;
  /* the members following this point are opaque */
  uint32_t slot;
  void *ptr;
} w_ht_iter_t;

/* Begin iterating the contents of the hash table.
 * Usage is:
 *     if (w_ht_first(ht, &iter)) do {
 *     } while (w_ht_next(ht, &iter));
 *
 * You may read the contents of iter.key and iter.value.
 * Note that these expose the raw values stored in the table;
 * they are not copies managed by the copy_key or copy_val
 * functions.
 *
 * Returns true if iterator holds a value key/value.
 * Returns false otherwise, indicating that the table is
 * either NULL or has no contents.
 *
 * It is not safe to call w_ht_del() while traversing
 * the hash table with this function; use w_ht_iter_del()
 * instead.
 */
bool w_ht_first(w_ht_t *ht, w_ht_iter_t *iter);

/* Advance to the next element in the table.
 * Returns false if there are no more elements.
 * See w_ht_first() for more information */
bool w_ht_next(w_ht_t *ht, w_ht_iter_t *iter);

/* Deletes the value associated with the current
 * iterator position and updates the iterator
 * so that the next call to w_ht_next() will see
 * the next value.
 * Returns true if successful, false if the iterator
 * was invalid */
bool w_ht_iter_del(w_ht_t *ht, w_ht_iter_t *iter);


/* helper functions for constructing hash tables with string keys */
w_ht_val_t w_ht_string_copy(w_ht_val_t val);
void w_ht_string_del(w_ht_val_t val);
bool w_ht_string_equal(w_ht_val_t a, w_ht_val_t b);
uint32_t w_ht_string_hash(w_ht_val_t val);

/* if you're building a hash table that uses w_string_t as keys,
 * then you can use w_ht_string_funcs as the second parameter
 * to w_ht_new to safely reference the keys as the table is updated.
 */
extern const struct watchman_hash_funcs w_ht_string_funcs;



#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

