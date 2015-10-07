/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct watchman_dir_handle {
  DIR *d;
  struct watchman_dir_ent ent;
};

/* Opens a directory making sure it's not a symlink */
DIR *opendir_nofollow(const char *path)
{
#ifdef _WIN32
  return win_opendir(path, 1 /* no follow */);
#else
  int fd = open(path, O_NOFOLLOW | O_CLOEXEC);
  if (fd == -1) {
    return NULL;
  }
#if defined(__APPLE__)
  close(fd);
  return opendir(path);
#else
  // errno should be set appropriately if this is not a directory
  return fdopendir(fd);
#endif
#endif
}

struct watchman_dir_handle *w_dir_open(const char *path) {
  struct watchman_dir_handle *dir = calloc(1, sizeof(*dir));
  int err;

  if (!dir) {
    return NULL;
  }
  dir->d = opendir_nofollow(path);

  if (!dir->d) {
    err = errno;
    free(dir);
    errno = err;
    return NULL;
  }

  return dir;
}

struct watchman_dir_ent *w_dir_read(struct watchman_dir_handle *dir) {
  struct dirent *ent;

  if (!dir->d) {
    return NULL;
  }

  ent = readdir(dir->d);
  if (!ent) {
    return NULL;
  }

  dir->ent.d_name = ent->d_name;
  dir->ent.has_stat = false;
  return &dir->ent;
}

void w_dir_close(struct watchman_dir_handle *dir) {
  closedir(dir->d);
  free(dir);
}

int w_dir_fd(struct watchman_dir_handle *dir) {
  return dirfd(dir->d);
}

/* vim:ts=2:sw=2:et:
 */
