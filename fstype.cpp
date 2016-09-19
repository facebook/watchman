/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_SYS_VFS_H
# include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_STATVFS_H
# include <sys/statvfs.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
# include <sys/mount.h>
#endif
#ifdef __linux__
#include <linux/magic.h>
#endif

// The primary purpose of checking the filesystem type is to prevent
// watching filesystems that are known to be problematic, such as
// network or remote mounted filesystems.  As such, we don't strictly
// need to have a fully comprehensive mapping of the underlying filesystem
// type codes to names, just the known problematic types

w_string w_fstype(const char *path)
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

  return w_string(name, W_STRING_UNICODE);
#elif STATVFS_HAS_FSTYPE_AS_STRING
  struct statvfs sfs;

  if (statvfs(path, &sfs) == 0) {
#ifdef HAVE_STRUCT_STATVFS_F_FSTYPENAME
    return w_string(sfs.f_fstypename, W_STRING_UNICODE);
#endif
#ifdef HAVE_STRUCT_STATVFS_F_BASETYPE
    return w_string(sfs.f_basetype, W_STRING_UNICODE);
#endif
  }
#elif HAVE_STATFS
  struct statfs sfs;

  if (statfs(path, &sfs) == 0) {
    return w_string(sfs.f_fstypename, W_STRING_UNICODE);
  }
#endif
#ifdef _WIN32
  WCHAR *wpath = w_utf8_to_win_unc(path, -1);
  w_string_t *fstype_name = NULL;
  if (wpath) {
    WCHAR fstype[MAX_PATH+1];
    HANDLE h = CreateFileW(wpath, GENERIC_READ,
        FILE_SHARE_DELETE|FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h) {
      if (GetVolumeInformationByHandleW(h, NULL, 0, 0, 0, 0, fstype,
            MAX_PATH+1)) {
        fstype_name = w_string_new_wchar_typed(fstype, -1, W_STRING_UNICODE);
      }
      CloseHandle(h);
    }
    free(wpath);
  }
  if (fstype_name) {
    return w_string(fstype_name, false);
  }
  return w_string("unknown", W_STRING_UNICODE);
#else
  unused_parameter(path);
  return w_string("unknown", W_STRING_UNICODE);
#endif
}

/* vim:ts=2:sw=2:et:
 */
