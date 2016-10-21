/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
# include <sys/utsname.h>
# include <sys/attr.h>
# include <sys/vnode.h>
#endif

#ifdef HAVE_GETATTRLISTBULK
typedef struct {
  uint32_t len;
  attribute_set_t returned;
  uint32_t err;

  /* The attribute data length will not be greater than NAME_MAX + 1
   * characters, which is NAME_MAX * 3 + 1 bytes (as one UTF-8-encoded
   * character may take up to three bytes
   */
  attrreference_t name; // ATTR_CMN_NAME
  dev_t dev;            // ATTR_CMN_DEVID
  fsobj_type_t objtype; // ATTR_CMN_OBJTYPE
  struct timespec mtime; // ATTR_CMN_MODTIME
  struct timespec ctime; // ATTR_CMN_CHGTIME
  struct timespec atime; // ATTR_CMN_ACCTIME
  uid_t uid; // ATTR_CMN_OWNERID
  gid_t gid; // ATTR_CMN_GRPID
  uint32_t mode; // ATTR_CMN_ACCESSMASK, Only the permission bits of st_mode
                 // are valid; other bits should be ignored,
                 // e.g., by masking with ~S_IFMT.
  uint64_t ino;  // ATTR_CMN_FILEID
  uint32_t link; // ATTR_FILE_LINKCOUNT or ATTR_DIR_LINKCOUNT
  off_t file_size; // ATTR_FILE_TOTALSIZE

} __attribute__((packed)) bulk_attr_item;
#endif

struct watchman_dir_handle {
#ifdef HAVE_GETATTRLISTBULK
  int fd;
  struct attrlist attrlist;
  int retcount;
  char buf[64 * (sizeof(bulk_attr_item) + NAME_MAX * 3 + 1)];
  char *cursor;
#endif
  DIR *d;
  struct watchman_dir_ent ent;
};

#ifdef _WIN32
static const char *w_basename(const char *path) {
  const char *last = path + strlen(path) - 1;
  while (last >= path) {
    if (*last == '/' || *last == '\\') {
      return last + 1;
    }
    --last;
  }
  return NULL;
}
#endif

static bool is_basename_canonical_case(const char *path) {
#ifdef __APPLE__
  struct attrlist attrlist;
  struct {
    uint32_t len;
    attrreference_t ref;
    char canonical_name[WATCHMAN_NAME_MAX];
  } vomit;
  char *name;
  const char *base = strrchr(path, '/') + 1;

  memset(&attrlist, 0, sizeof(attrlist));
  attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrlist.commonattr = ATTR_CMN_NAME;

  if (getattrlist(path, &attrlist, &vomit,
        sizeof(vomit), FSOPT_NOFOLLOW) == -1) {
    w_log(W_LOG_ERR, "getattrlist(%s) failed: %s\n",
        path, strerror(errno));
    return false;
  }

  name = ((char*)&vomit.ref) + vomit.ref.attr_dataoffset;
  return strcmp(name, base) == 0;
#elif defined(_WIN32)
  WCHAR long_buf[WATCHMAN_NAME_MAX];
  WCHAR *wpath = w_utf8_to_win_unc(path, -1);
  DWORD err;
  char *canon;
  bool result;
  const char *path_base;
  const char *canon_base;

  DWORD long_len = GetLongPathNameW(wpath, long_buf,
                      sizeof(long_buf)/sizeof(long_buf[0]));
  err = GetLastError();
  free(wpath);

  if (long_len == 0 && err == ERROR_FILE_NOT_FOUND) {
    // signal to caller that the file has disappeared -- the caller will read
    // errno and do error handling
    errno = map_win32_err(err);
    return false;
  }

  if (long_len == 0) {
    w_log(W_LOG_ERR, "Failed to canon(%s): %s\n", path, win32_strerror(err));
    return false;
  }

  if (long_len > sizeof(long_buf)-1) {
    w_log(W_LOG_FATAL, "GetLongPathNameW needs %lu chars\n", long_len);
  }

  canon = w_win_unc_to_utf8(long_buf, long_len, NULL);
  if (!canon) {
    return false;
  }

  path_base = w_basename(path);
  canon_base = w_basename(canon);
  if (!path_base || !canon_base) {
    result = false;
  } else {
    result = strcmp(path_base, canon_base) == 0;
    if (!result) {
      w_log(W_LOG_ERR,
            "is_basename_canonical_case(%s): basename=%s != canonical "
            "%s basename of %s\n",
            path, path_base, canon, canon_base);
    }
  }
  free(canon);

  return result;
#else
  unused_parameter(path);
  return true;
#endif
}

/* This function always returns a buffer that needs to
 * be released via free(3).  We use the native feature
 * of the system libc if we know it is present, otherwise
 * we need to malloc a buffer for ourselves.  This
 * is made more fun because some systems have a dynamic
 * buffer size obtained via sysconf().
 */
char *w_realpath(const char *filename) {
#if defined(__GLIBC__) || defined(__APPLE__) || defined(_WIN32)
  return realpath(filename, NULL);
#else
  char *buf = NULL;
  char *retbuf;
  int path_max = 0;

#ifdef _SC_PATH_MAX
  path_max = sysconf(path, _SC_PATH_MAX);
#endif
  if (path_max <= 0) {
    path_max = WATCHMAN_NAME_MAX;
  }
  buf = (char*)malloc(path_max);
  if (!buf) {
    return NULL;
  }

  retbuf = realpath(filename, buf);

  if (retbuf != buf) {
    free(buf);
    return NULL;
  }

  return retbuf;
#endif
}

/* Extract the canonical path of an open file descriptor and store
 * it into the provided buffer.
 * Unlike w_realpath, which will return an allocated buffer of the correct
 * size, this will indicate EOVERFLOW if the buffer is too small.
 * For our purposes this is fine: if the canonical name is larger than
 * the input buffer it means that it does not match the path that we
 * wanted to open; we don't care what the actual canonical path really is.
 */
static int realpath_fd(int fd, char *canon, size_t canon_size) {
#if defined(F_GETPATH)
  unused_parameter(canon_size);
  return fcntl(fd, F_GETPATH, canon);
#elif defined(__linux__)
  char procpath[1024];
  int len;

  snprintf(procpath, sizeof(procpath), "/proc/%d/fd/%d", getpid(), fd);
  len = readlink(procpath, canon, canon_size);
  if (len > 0) {
    canon[len] = 0;
    return 0;
  }
  if (errno == ENOENT) {
    // procfs is likely not mounted, let caller fall back to realpath()
    errno = ENOSYS;
  }
  return -1;
#elif defined(_WIN32)
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  WCHAR final_buf[WATCHMAN_NAME_MAX];
  DWORD len, err;
  DWORD nchars = sizeof(final_buf) / sizeof(WCHAR);

  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }

  len = GetFinalPathNameByHandleW(h, final_buf, nchars, FILE_NAME_NORMALIZED);
  err = GetLastError();
  if (len >= nchars) {
    errno = EOVERFLOW;
    return -1;
  }
  if (len > 0) {
    uint32_t utf_len;
    char *utf8 = w_win_unc_to_utf8(final_buf, len, &utf_len);
    if (!utf8) {
      return -1;
    }

    if (utf_len + 1 > canon_size) {
      free(utf8);
      errno = EOVERFLOW;
      return -1;
    }

    memcpy(canon, utf8, utf_len + 1);
    free(utf8);
    return 0;
  }

  errno = map_win32_err(err);
  return -1;
#else
  unused_parameter(canon);
  unused_parameter(canon_size);
  errno = ENOSYS;
  return -1;
#endif
}

/* Opens a file or directory, strictly prohibiting opening any symlinks
 * in any component of the path, and strictly matching the canonical
 * case of the file for case insensitive filesystems.
 */
static int open_strict(const char *path, int flags) {
  int fd;
  char canon[WATCHMAN_NAME_MAX];
  int err = 0;
  char *pathcopy = NULL;

  if (strlen(path) >= sizeof(canon)) {
    w_log(W_LOG_ERR, "open_strict(%s): path is larger than WATCHMAN_NAME_MAX\n",
          path);
    errno = EOVERFLOW;
    return -1;
  }

  fd = open(path, flags);
  if (fd == -1) {
    return -1;
  }

  if (realpath_fd(fd, canon, sizeof(canon)) == 0) {
    if (strcmp(canon, path) == 0) {
      return fd;
    }

    w_log(W_LOG_DBG, "open_strict(%s): doesn't match canon path %s\n",
        path, canon);
    close(fd);
    errno = ENOENT;
    return -1;
  }

  if (errno != ENOSYS) {
    err = errno;
    w_log(W_LOG_ERR, "open_strict(%s): realpath_fd failed: %s\n", path,
          strerror(err));
    close(fd);
    errno = err;
    return -1;
  }

  // Fall back to realpath
  pathcopy = w_realpath(path);
  if (!pathcopy) {
    err = errno;
    w_log(W_LOG_ERR, "open_strict(%s): realpath failed: %s\n", path,
          strerror(err));
    close(fd);
    errno = err;
    return -1;
  }

  if (strcmp(pathcopy, path)) {
    // Doesn't match canonical case or the path we were expecting
    w_log(W_LOG_ERR, "open_strict(%s): canonical path is %s\n",
        path, pathcopy);
    free(pathcopy);
    close(fd);
    errno = ENOENT;
    return -1;
  }
  free(pathcopy);
  return fd;
}

// Like lstat, but strict about symlinks in any path component
int w_lstat(const char *path, struct stat *st, bool case_sensitive) {
  char *parent, *slash;
  int fd = -1;
  int err, res = -1;

  parent = strdup(path);
  if (!parent) {
    return -1;
  }

  slash = strrchr(parent, '/');
  if (slash) {
    *slash = '\0';
  }
  fd = open_strict(parent, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
  if (fd == -1) {
    err = errno;
    goto out;
  }

  errno = 0;
#ifdef HAVE_OPENAT
  res = fstatat(fd, slash + 1, st, AT_SYMLINK_NOFOLLOW);
  err = errno;
#else
  res = lstat(path, st);
  err = errno;
#endif

  if (res == 0 && !case_sensitive && !is_basename_canonical_case(path)) {
    res = -1;
    err = ENOENT;
  }

out:
  if (fd != -1) {
    close(fd);
  }
  free(parent);
  errno = err;
  return res;
}

/* Opens a directory making sure it's not a symlink */
static DIR *opendir_nofollow(const char *path)
{
  int fd = open_strict(path, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd == -1) {
    return NULL;
  }
#ifdef _WIN32
  close(fd);
  return win_opendir(path, 1 /* no follow */);
#else
# if !defined(HAVE_FDOPENDIR) || defined(__APPLE__)
  /* fdopendir doesn't work on earlier versions OS X, and we don't
   * use this function since 10.10, as we prefer to use getattrlistbulk
   * in that case */
  close(fd);
  return opendir(path);
# else
  // errno should be set appropriately if this is not a directory
  return fdopendir(fd);
# endif
#endif
}

#ifdef HAVE_GETATTRLISTBULK
// I've seen bulkstat report incorrect sizes on kernel version 14.5.0.
// (That's OSX 10.10.5).
// Let's avoid it for major kernel versions < 15.
// Using statics here to avoid querying the uname on every opendir.
// There is opportunity for a data race the first time through, but the
// worst case side effect is wasted compute early on.
static bool use_bulkstat_by_default(void) {
  static bool probed = false;
  static bool safe = false;

  if (!probed) {
    struct utsname name;
    if (uname(&name) == 0) {
      int maj = 0, min = 0, patch = 0;
      sscanf(name.release, "%d.%d.%d", &maj, &min, &patch);
      if (maj >= 15) {
        safe = true;
      }
    }
    probed = true;
  }

  return safe;
}
#endif

struct watchman_dir_handle *w_dir_open(const char *path) {
  watchman_dir_handle* dir = (watchman_dir_handle*)calloc(1, sizeof(*dir));
  int err;

  if (!dir) {
    return NULL;
  }
#ifdef HAVE_GETATTRLISTBULK
  if (cfg_get_bool("_use_bulkstat", use_bulkstat_by_default())) {
    struct stat st;

    dir->fd = open_strict(path, O_NOFOLLOW | O_CLOEXEC | O_RDONLY);
    if (dir->fd == -1) {
      err = errno;
      free(dir);
      errno = err;
      return NULL;
    }

    if (fstat(dir->fd, &st)) {
      err = errno;
      close(dir->fd);
      free(dir);
      errno = err;
      return NULL;
    }

    if (!S_ISDIR(st.st_mode)) {
      close(dir->fd);
      free(dir);
      errno = ENOTDIR;
      return NULL;
    }

    dir->attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
    dir->attrlist.commonattr = ATTR_CMN_RETURNED_ATTRS |
      ATTR_CMN_ERROR |
      ATTR_CMN_NAME |
      ATTR_CMN_DEVID |
      ATTR_CMN_OBJTYPE |
      ATTR_CMN_MODTIME |
      ATTR_CMN_CHGTIME |
      ATTR_CMN_ACCTIME |
      ATTR_CMN_OWNERID |
      ATTR_CMN_GRPID |
      ATTR_CMN_ACCESSMASK |
      ATTR_CMN_FILEID;
    dir->attrlist.dirattr = ATTR_DIR_LINKCOUNT;
    dir->attrlist.fileattr = ATTR_FILE_TOTALSIZE |
      ATTR_FILE_LINKCOUNT;
    return dir;
  }
  dir->fd = -1;
#endif
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
#ifdef HAVE_GETATTRLISTBULK
  if (dir->fd != -1) {
    bulk_attr_item *item;

    if (!dir->cursor) {
      // Read the next batch of results
      int retcount;

      retcount = getattrlistbulk(dir->fd, &dir->attrlist,
          dir->buf, sizeof(dir->buf), FSOPT_PACK_INVAL_ATTRS);
      if (retcount == -1) {
        int err = errno;
        w_log(W_LOG_ERR, "getattrlistbulk: error %d %s\n",
            errno, strerror(err));
        errno = err;
        return NULL;
      }
      if (retcount == 0) {
        // End of the stream
        errno = 0;
        return NULL;
      }

      dir->retcount = retcount;
      dir->cursor = dir->buf;
    }

    // Decode the next item
    item = (bulk_attr_item*)dir->cursor;
    dir->cursor += item->len;
    if (--dir->retcount == 0) {
      dir->cursor = NULL;
    }

    dir->ent.d_name = ((char*)&item->name) + item->name.attr_dataoffset;
    if (item->err) {
      w_log(W_LOG_ERR, "item error %s: %d %s\n", dir->ent.d_name,
          item->err, strerror(item->err));
      // We got the name, so we can return something useful
      dir->ent.has_stat = false;
      return &dir->ent;
    }

    memset(&dir->ent.stat, 0, sizeof(dir->ent.stat));

    dir->ent.stat.dev = item->dev;
    memcpy(&dir->ent.stat.mtime, &item->mtime, sizeof(item->mtime));
    memcpy(&dir->ent.stat.ctime, &item->ctime, sizeof(item->ctime));
    memcpy(&dir->ent.stat.atime, &item->atime, sizeof(item->atime));
    dir->ent.stat.uid = item->uid;
    dir->ent.stat.gid = item->gid;
    dir->ent.stat.mode = item->mode & ~S_IFMT;
    dir->ent.stat.ino = item->ino;

    switch (item->objtype) {
      case VREG:
        dir->ent.stat.mode |= S_IFREG;
        dir->ent.stat.size = item->file_size;
        dir->ent.stat.nlink = item->link;
        break;
      case VDIR:
        dir->ent.stat.mode |= S_IFDIR;
        dir->ent.stat.nlink = item->link;
        break;
      case VLNK:
        dir->ent.stat.mode |= S_IFLNK;
        dir->ent.stat.size = item->file_size;
        break;
      case VBLK:
        dir->ent.stat.mode |= S_IFBLK;
        break;
      case VCHR:
        dir->ent.stat.mode |= S_IFCHR;
        break;
      case VFIFO:
        dir->ent.stat.mode |= S_IFIFO;
        break;
      case VSOCK:
        dir->ent.stat.mode |= S_IFSOCK;
        break;
    }
    dir->ent.has_stat = true;
    return &dir->ent;
  }
#endif

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
#ifdef HAVE_GETATTRLISTBULK
  if (dir->fd != -1) {
    close(dir->fd);
  }
#endif
  if (dir->d) {
    closedir(dir->d);
    dir->d = NULL;
  }
  free(dir);
}

#ifndef _WIN32
int w_dir_fd(struct watchman_dir_handle *dir) {
#ifdef HAVE_GETATTRLISTBULK
  return dir->fd;
#else
  return dirfd(dir->d);
#endif
}
#endif

/* vim:ts=2:sw=2:et:
 */
