/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
