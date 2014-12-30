/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef DIRENT_H
#define DIRENT_H

struct dirent {
  char d_name[MAX_PATH];
};
typedef struct watchman_win32_dir {
  HANDLE h;
  FILE_FULL_DIR_INFO *info;
  char __declspec(align(8)) buf[64*1024];
  struct dirent ent;
} DIR;

DIR *win_opendir(const char *path, int nofollow);
DIR *opendir(const char *path);
void closedir(DIR *d);
struct dirent *readdir(DIR *d);

#endif

/* vim:ts=2:sw=2:et:
 */
