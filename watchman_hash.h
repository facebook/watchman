/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_HASH_H
#define WATCHMAN_HASH_H

/* Bob Jenkins' lookup3.c hash function */
uint32_t w_hash_bytes(const void* key, size_t length, uint32_t initval);

#endif

/* vim:ts=2:sw=2:et:
 */
