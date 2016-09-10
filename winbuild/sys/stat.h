/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int mode_t;
typedef int uid_t;
typedef int gid_t;
typedef int nlink_t;

// FIXME: don't do this, define our own layer
#define _STAT_DEFINED
struct stat {
  uint64_t st_size;
  mode_t st_mode;
  struct timespec st_atim, st_mtim, st_ctim;
  time_t st_atime, st_mtime, st_ctime;
  uid_t st_uid;
  gid_t st_gid;
  ino_t st_ino;
  dev_t st_dev;
  nlink_t st_nlink;
  dev_t st_rdev;
};

int lstat(const char *path, struct stat *st);
int mkdir(const char *path, int mode);
int open_and_share(const char *path, int flags, ...);
#define open open_and_share

#define S_ISUID       0004000     ///< set user id on execution
#define S_ISGID       0002000     ///< set group id on execution
#define S_ISTXT       0001000     ///< sticky bit

#define S_IRWXU       0000700     ///< RWX mask for owner
#define S_IRUSR       0000400     ///< R for owner
#define S_IWUSR       0000200     ///< W for owner
#define S_IXUSR       0000100     ///< X for owner

#define S_IREAD       S_IRUSR
#define S_IWRITE      S_IWUSR
#define S_IEXEC       S_IXUSR

#define S_IRWXG       0000070     ///< RWX mask for group
#define S_IRGRP       0000040     ///< R for group
#define S_IWGRP       0000020     ///< W for group
#define S_IXGRP       0000010     ///< X for group

#define S_IRWXO       0000007     ///< RWX mask for other
#define S_IROTH       0000004     ///< R for other
#define S_IWOTH       0000002     ///< W for other
#define S_IXOTH       0000001     ///< X for other

/*  The Octal access modes, above, fall into the Hex mask 0x00000FFF.
    Traditionally, the remainder of the flags are specified in Octal
    but they are expressed in Hex here for modern clarity.
*/
#define _S_IFMT       0x000FF000   ///< type-of-file mask
#define _S_IFIFO      0x00001000   ///< named pipe (fifo)
#define _S_IFCHR      0x00002000   ///< character special
#define _S_IFDIR      0x00004000   ///< directory
#define _S_IFBLK      0x00006000   ///< block special
#define _S_IFREG      0x00008000   ///< regular
#define _S_IFSOCK     0x0000C000   ///< socket

#define S_IFMT   _S_IFMT
#define S_IFBLK  _S_IFBLK
#define S_IFREG  _S_IFREG
#define S_IFIFO  _S_IFIFO
#define S_IFCHR  _S_IFCHR
#define S_IFDIR  _S_IFDIR
#define S_IFSOCK _S_IFSOCK

#define S_ISDIR(m)  ((m & _S_IFMT) == _S_IFDIR)   ///< directory
#define S_ISCHR(m)  ((m & _S_IFMT) == _S_IFCHR)   ///< char special
#define S_ISREG(m)  ((m & _S_IFMT) == _S_IFREG)   ///< regular file
#define S_ISBLK(m)  ((m & _S_IFMT) == _S_IFBLK)   ///< block special
#define S_ISSOCK(m) ((m & _S_IFMT) == _S_IFSOCK)  ///< socket

#define S_ISFIFO(m) ((m & _S_IFMT) == _S_IFIFO)   ///< fifo
#define S_ISLNK(m) 0

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
