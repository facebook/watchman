/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef __APPLE__
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
#ifdef HAVE_GETATTRLISTBULK
  // This option is here temporarily in case we discover problems with
  // bulkstat and need to disable it.  We will remove this option in
  // a future release of watchman
  if (cfg_get_bool(NULL, "_use_bulkstat", true)) {
    dir->fd = open(path, O_NOFOLLOW | O_CLOEXEC | O_RDONLY);
    if (dir->fd == -1) {
      err = errno;
      free(dir);
      errno = err;
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
  w_log(W_LOG_ERR, "Using opendir\n");
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
        w_log(W_LOG_ERR, "getattrlistbulk: error %d %s\n",
            errno, strerror(errno));
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
