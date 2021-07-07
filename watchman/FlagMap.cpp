/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

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
