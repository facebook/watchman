/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <system_error>
#ifndef _WIN32
#include <dirent.h>
#endif
#ifdef __APPLE__
# include <sys/utsname.h>
# include <sys/attr.h>
# include <sys/vnode.h>
#endif
#include "FileDescriptor.h"

using watchman::FileDescriptor;

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

#ifndef _WIN32
class DirHandle : public watchman_dir_handle {
#ifdef HAVE_GETATTRLISTBULK
  FileDescriptor fd_;
  struct attrlist attrlist_;
  int retcount_{0};
  char buf_[64 * (sizeof(bulk_attr_item) + NAME_MAX * 3 + 1)];
  char *cursor_{nullptr};
#endif
  DIR *d_{nullptr};
  struct watchman_dir_ent ent_;

 public:
  explicit DirHandle(const char *path);
  ~DirHandle();
  const watchman_dir_ent* readDir() override;
  int getFd() const override;
};
#else
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
  std::wstring long_buf;
  auto wpath = w_string_piece(path).asWideUNC();
  DWORD err;
  bool result;

  long_buf.resize(WATCHMAN_NAME_MAX);
  DWORD long_len =
      GetLongPathNameW(wpath.c_str(), &long_buf[0], long_buf.size());
  err = GetLastError();

  if (long_len == 0) {
    throw std::system_error(err, std::system_category(), "GetLongPathNameW");
  }

  if (long_len > long_buf.size()) {
    long_buf.resize(long_len);
    long_len = GetLongPathNameW(wpath.c_str(), &long_buf[0], long_buf.size());
    err = GetLastError();
    if (long_len == 0) {
      throw std::system_error(err, std::system_category(), "GetLongPathNameW");
    }
  }

  w_string canon(long_buf.data(), long_len);

  auto path_base = w_string_piece(path).baseName();
  auto canon_base = canon.piece().baseName();

  if (!path_base.size() || !canon_base.size()) {
    result = false;
  } else {
    result = path_base == canon_base;
    if (!result) {
      w_log(W_LOG_ERR,
            "is_basename_canonical_case(%s): basename=%s != canonical "
            "%s basename of %s\n",
            path, path_base, canon, canon_base);
    }
  }

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
    w_string utf8(final_buf, len);

    if (utf8.size() + 1 > canon_size) {
      errno = EOVERFLOW;
      return -1;
    }

    memcpy(canon, utf8.data(), utf8.size() + 1);
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

#ifndef _WIN32
/* Opens a directory making sure it's not a symlink */
static DIR *opendir_nofollow(const char *path)
{
  int fd = open_strict(path, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd == -1) {
    return NULL;
  }
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
}
#endif

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

#ifndef _WIN32
std::unique_ptr<watchman_dir_handle> w_dir_open(const char* path) {
  return watchman::make_unique<DirHandle>(path);
}

DirHandle::DirHandle(const char *path) {
#ifdef HAVE_GETATTRLISTBULK
  if (cfg_get_bool("_use_bulkstat", use_bulkstat_by_default())) {
    struct stat st;

    fd_ = FileDescriptor(open_strict(path, O_NOFOLLOW | O_CLOEXEC | O_RDONLY));
    if (!fd_) {
      throw std::system_error(
          errno, std::generic_category(), std::string("opendir ") + path);
    }

    if (fstat(fd_.fd(), &st)) {
      throw std::system_error(
          errno, std::generic_category(), std::string("fstat ") + path);
    }

    if (!S_ISDIR(st.st_mode)) {
      throw std::system_error(ENOTDIR, std::generic_category(), path);
    }

    memset(&attrlist_, 0, sizeof(attrlist_));
    attrlist_.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrlist_.commonattr = ATTR_CMN_RETURNED_ATTRS |
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
    attrlist_.dirattr = ATTR_DIR_LINKCOUNT;
    attrlist_.fileattr = ATTR_FILE_TOTALSIZE | ATTR_FILE_LINKCOUNT;
    return;
  }
#endif
  d_ = opendir_nofollow(path);

  if (!d_) {
    throw std::system_error(
        errno,
        std::generic_category(),
        std::string("opendir_nofollow: ") + path);
  }
}

const watchman_dir_ent* DirHandle::readDir() {
#ifdef HAVE_GETATTRLISTBULK
  if (fd_) {
    bulk_attr_item *item;

    if (!cursor_) {
      // Read the next batch of results
      int retcount;

      errno = 0;
      retcount = getattrlistbulk(
          fd_.fd(), &attrlist_, buf_, sizeof(buf_), FSOPT_PACK_INVAL_ATTRS);
      if (retcount == -1) {
        throw std::system_error(
            errno, std::generic_category(), "getattrlistbulk");
      }
      if (retcount == 0) {
        // End of the stream
        return nullptr;
      }

      retcount_ = retcount;
      cursor_ = buf_;
    }

    // Decode the next item
    item = (bulk_attr_item*)cursor_;
    cursor_ += item->len;
    if (--retcount_ == 0) {
      cursor_ = nullptr;
    }

    ent_.d_name = ((char*)&item->name) + item->name.attr_dataoffset;
    if (item->err) {
      w_log(
          W_LOG_ERR,
          "item error %s: %d %s\n",
          ent_.d_name,
          item->err,
          strerror(item->err));
      // We got the name, so we can return something useful
      ent_.has_stat = false;
      return &ent_;
    }

    memset(&ent_.stat, 0, sizeof(ent_.stat));

    ent_.stat.dev = item->dev;
    memcpy(&ent_.stat.mtime, &item->mtime, sizeof(item->mtime));
    memcpy(&ent_.stat.ctime, &item->ctime, sizeof(item->ctime));
    memcpy(&ent_.stat.atime, &item->atime, sizeof(item->atime));
    ent_.stat.uid = item->uid;
    ent_.stat.gid = item->gid;
    ent_.stat.mode = item->mode & ~S_IFMT;
    ent_.stat.ino = item->ino;

    switch (item->objtype) {
      case VREG:
        ent_.stat.mode |= S_IFREG;
        ent_.stat.size = item->file_size;
        ent_.stat.nlink = item->link;
        break;
      case VDIR:
        ent_.stat.mode |= S_IFDIR;
        ent_.stat.nlink = item->link;
        break;
      case VLNK:
        ent_.stat.mode |= S_IFLNK;
        ent_.stat.size = item->file_size;
        break;
      case VBLK:
        ent_.stat.mode |= S_IFBLK;
        break;
      case VCHR:
        ent_.stat.mode |= S_IFCHR;
        break;
      case VFIFO:
        ent_.stat.mode |= S_IFIFO;
        break;
      case VSOCK:
        ent_.stat.mode |= S_IFSOCK;
        break;
    }
    ent_.has_stat = true;
    return &ent_;
  }
#endif

  if (!d_) {
    return nullptr;
  }
  errno = 0;
  auto dent = readdir(d_);
  if (!dent) {
    if (errno) {
      throw std::system_error(errno, std::generic_category(), "readdir");
    }
    return nullptr;
  }

  ent_.d_name = dent->d_name;
  ent_.has_stat = false;
  return &ent_;
}

DirHandle::~DirHandle() {
  if (d_) {
    closedir(d_);
  }
}

int DirHandle::getFd() const {
#ifdef HAVE_GETATTRLISTBULK
  return fd_.fd();
#else
  return dirfd(d_);
#endif
}
#endif

/* vim:ts=2:sw=2:et:
 */
