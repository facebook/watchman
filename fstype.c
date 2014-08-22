/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_SYS_VFS_H
# include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_STATVFS_H
# include <sys/statvfs.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
# include <sys/mount.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef __linux__
#include <linux/magic.h>
#endif

// The primary purpose of checking the filesystem type is to prevent
// watching filesystems that are known to be problematic, such as
// network or remote mounted filesystems.  As such, we don't strictly
// need to have a fully comprehensive mapping of the underlying filesystem
// type codes to names, just the known problematic types

w_string_t *w_fstype(const char *path)
{
#ifdef __linux__
  struct statfs sfs;
  const char *name = "unknown";

  if (statfs(path, &sfs) == 0) {
    switch (sfs.f_type) {
#ifdef CIFS_MAGIC_NUMBER
      case CIFS_MAGIC_NUMBER:
        name = "cifs";
        break;
#endif
#ifdef NFS_SUPER_MAGIC
      case NFS_SUPER_MAGIC:
        name = "nfs";
        break;
#endif
#ifdef SMB_SUPER_MAGIC
      case SMB_SUPER_MAGIC:
        name = "smb";
        break;
#endif
      default:
        name = "unknown";
    }
  }

  return w_string_new(name);
#elif HAVE_SYS_STATVFS_H && !defined(__APPLE__)
  struct statvfs sfs;

  if (statvfs(path, &sfs) == 0) {
#ifdef __NetBSD__
    return w_string_new(sfs.f_fstypename);
#else
    return w_string_new(sfs.f_basetype);
#endif
  }
#elif HAVE_STATFS
  struct statfs sfs;

  if (statfs(path, &sfs) == 0) {
    return w_string_new(sfs.f_fstypename);
  }
#endif
  return w_string_new("unknown");
}

/* vim:ts=2:sw=2:et:
 */
