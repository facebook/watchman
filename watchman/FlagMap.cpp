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

#include "watchman/FlagMap.h"
#include <string.h>
#include <algorithm>

void w_expand_flags(
    const struct flag_map* fmap,
    uint32_t flags,
    char* buf,
    size_t len) {
  bool first = true;
  *buf = '\0';
  while (fmap->label && len) {
    if ((flags & fmap->value) == fmap->value) {
      size_t space;

      if (!first) {
        *buf = ' ';
        buf++;
        len--;
      } else {
        first = false;
      }

      space = std::min(len, strlen(fmap->label) + 1);
      memcpy(buf, fmap->label, space);

      len -= space - 1;
      buf += space - 1;
    }
    fmap++;
  }
}

/* vim:ts=2:sw=2:et:
 */
