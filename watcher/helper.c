/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

bool w_is_ignored(w_root_t *root, const char *path, uint32_t pathlen)
{
  return w_ignore_check(&root->ignore, path, pathlen);
}
