/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

struct watchman_stat {
  struct timespec atime, mtime, ctime;
  off_t size;
  mode_t mode;
  uid_t uid;
  gid_t gid;
  ino_t ino;
  dev_t dev;
  nlink_t nlink;
};

/* opaque (system dependent) type for walking dir contents */
struct watchman_dir_handle;

struct watchman_dir_ent {
  bool has_stat;
  char *d_name;
  struct watchman_stat stat;
};

struct watchman_dir_handle *w_dir_open(const char *path);
struct watchman_dir_ent *w_dir_read(struct watchman_dir_handle *dir);
void w_dir_close(struct watchman_dir_handle *dir);
int w_dir_fd(struct watchman_dir_handle *dir);
