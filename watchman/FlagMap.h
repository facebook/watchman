/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

struct flag_map {
  uint32_t value;
  const char* label;
};

/**
 * Given a flag map in `fmap`, and a set of flags in `flags`,
 * expand the flag bits that are set in `flags` into the corresponding
 * labels in `fmap` and print the result into the caller provided
 * buffer `buf` of size `len` bytes.
 */
void w_expand_flags(
    const struct flag_map* fmap,
    uint32_t flags,
    char* buf,
    size_t len);
