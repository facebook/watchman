/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "FileDescriptor.h"
#include "FileSystem.h"
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/utsname.h>
#include <sys/vnode.h>
#endif
#include <system_error>

#if defined(_WIN32) || defined(O_PATH)
#define CAN_OPEN_SYMLINKS 1
#else
#define CAN_OPEN_SYMLINKS 0
#endif

namespace watchman {

FileDescriptor::~FileDescriptor() {
  close();
}

FileDescriptor::FileDescriptor(int fd) : fd_(fd) {}

FileDescriptor::FileDescriptor(int fd, const char* operation) : fd_(fd) {
  if (fd_ == -1) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string(operation) + ": " + strerror(errno));
  }
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  close();
  fd_ = other.fd_;
  other.fd_ = -1;
  return *this;
}

void FileDescriptor::close() {
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

int FileDescriptor::release() {
  int result = fd_;
  fd_ = -1;
  return result;
}

void FileDescriptor::setCloExec() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFD, FD_CLOEXEC));
#endif
}

void FileDescriptor::setNonBlock() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK));
#endif
}

void FileDescriptor::clearNonBlock() {
#ifndef _WIN32
  ignore_result(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) & ~O_NONBLOCK));
#endif
}

bool FileDescriptor::isNonBlock() const {
#ifndef _WIN32
  return (fcntl(fd_, F_GETFL) & O_NONBLOCK) == O_NONBLOCK;
#else
  return false;
#endif
}

#if !CAN_OPEN_SYMLINKS
/** Checks that the basename component of the input path exactly
 * matches the canonical case of the path on disk.
 * It only makes sense to call this function on a case insensitive filesystem.
 * If the case does not match, throws an exception. */
static void checkCanonicalBaseName(const char *path) {
#ifdef __APPLE__
  struct attrlist attrlist;
  struct {
    uint32_t len;
    attrreference_t ref;
    char canonical_name[WATCHMAN_NAME_MAX];
  } vomit;
  w_string_piece pathPiece(path);
  auto base = pathPiece.baseName();

  memset(&attrlist, 0, sizeof(attrlist));
  attrlist.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrlist.commonattr = ATTR_CMN_NAME;

  if (getattrlist(path, &attrlist, &vomit, sizeof(vomit), FSOPT_NOFOLLOW) ==
      -1) {
    throw std::system_error(errno, std::generic_category(),
                            to<std::string>("checkCanonicalBaseName(", path,
                                            "): getattrlist failed"));
  }

  w_string_piece name(((char *)&vomit.ref) + vomit.ref.attr_dataoffset);
  if (name != base) {
    throw std::system_error(
        ENOENT, std::generic_category(),
        to<std::string>("checkCanonicalBaseName(", path, "): (", name,
                        ") doesn't match canonical base (", base, ")"));
  }
#else
  // Older Linux and BSDish systems are in this category.
  // This is the awful portable fallback used in the absence of
  // a system specific way to detect this.
  w_string_piece pathPiece(path);
  auto parent = pathPiece.dirName().asWString();
  auto dir = w_dir_open(parent.c_str());
  auto base = pathPiece.baseName();

  while (true) {
    auto ent = dir->readDir();
    if (!ent) {
      // We didn't find an entry that exactly matched -> fail
      throw std::system_error(
          ENOENT, std::generic_category(),
          to<std::string>("checkCanonicalBaseName(", path,
                          "): no match found in parent dir"));
    }
    // Note: we don't break out early if we get a case-insensitive match
    // because the dir may contain multiple representations of the same
    // name.  For example, Bash-for-Windows has dirs that contain both
    // "pod" and "Pod" dirs in its perl installation.  We want to make
    // sure that we've observed all of the entries in the dir before
    // giving up.
    if (w_string_piece(ent->d_name) == base) {
      // Exact match; all is good!
      return;
    }
  }
#endif
}
#endif

#ifndef _WIN32
FileDescriptor openFileHandle(const char *path,
                              const OpenFileHandleOptions &opts) {
  int flags = (!opts.followSymlinks ? O_NOFOLLOW : 0) |
              (opts.closeOnExec ? O_CLOEXEC : 0) |
#ifdef O_PATH
              (opts.metaDataOnly ? O_PATH : 0) |
#endif
              ((opts.readContents && opts.writeContents)
                   ? O_RDWR
                   : (opts.writeContents ? O_WRONLY
                                         : opts.readContents ? O_RDONLY : 0)) |
              (opts.create ? O_CREAT : 0) |
              (opts.exclusiveCreate ? O_EXCL : 0) |
              (opts.truncate ? O_TRUNC : 0);

  auto fd = open(path, flags);
  if (fd == -1) {
    int err = errno;
    throw std::system_error(
        err, std::generic_category(), to<std::string>("open: ", path));
  }
  FileDescriptor file(fd);

  if (!opts.strictNameChecks) {
    return file;
  }

  auto opened = file.getOpenedPath();
  if (w_string_piece(opened).pathIsEqual(path)) {
#if !CAN_OPEN_SYMLINKS
    CaseSensitivity caseSensitive = opts.caseSensitive;
    if (caseSensitive == CaseSensitivity::Unknown) {
      caseSensitive = getCaseSensitivityForPath(path);
    }
    if (caseSensitive == CaseSensitivity::CaseInSensitive) {
      // We need to perform one extra check for case-insensitive
      // paths to make sure that we didn't accidentally open
      // the wrong case name.
      checkCanonicalBaseName(path);
    }
#endif
    return file;
  }

  throw std::system_error(
      ENOENT, std::generic_category(),
      to<std::string>("open(", path,
                      "): opened path doesn't match canonical path ", opened));
}

FileInformation FileDescriptor::getInfo() const {
  struct stat st;
  if (fstat(fd_, &st)) {
    int err = errno;
    throw std::system_error(err, std::generic_category(), "fstat");
  }
  return FileInformation(st);
}

#endif

w_string FileDescriptor::getOpenedPath() const {
#if defined(F_GETPATH)
  // macOS.  The kernel interface only allows MAXPATHLEN
  char buf[MAXPATHLEN + 1];
  if (fcntl(fd_, F_GETPATH, buf) == -1) {
    throw std::system_error(errno, std::generic_category(),
                            "fcntl for getOpenedPath");
  }
  return w_string(buf);
#elif defined(__linux__)
  char procpath[1024];
  snprintf(procpath, sizeof(procpath), "/proc/%d/fd/%d", getpid(), fd_);

  // Avoid an extra stat by speculatively attempting to read into
  // a reasonably sized buffer.
  char buf[WATCHMAN_NAME_MAX];
  auto len = readlink(procpath, buf, sizeof(buf));
  if (len == sizeof(buf)) {
    len = -1;
    // We need to stat it to discover the required length
    errno = ENAMETOOLONG;
  }

  if (len >= 0) {
    return w_string(buf, len);
  }

  if (errno == ENOENT) {
    // For this path to not exist must mean that /proc is not mounted.
    // Report this with an actionable message
    throw std::system_error(ENOSYS, std::generic_category(),
                            "getOpenedPath: need /proc to be mounted!");
  }

  if (errno != ENAMETOOLONG) {
    throw std::system_error(errno, std::generic_category(),
                            "readlink for getOpenedPath");
  }

  // Figure out how much space we need
  struct stat st;
  if (fstat(fd_, &st)) {
    throw std::system_error(errno, std::generic_category(),
                            "fstat for getOpenedPath");
  }
  std::string result;
  result.resize(st.st_size + 1, 0);

  len = readlink(procpath, &result[0], result.size());
  if (len == int(result.size())) {
    // It's longer than we expected; TOCTOU detected!
    throw std::system_error(
        ENAMETOOLONG, std::generic_category(),
        "readlinkat: link contents grew while examining file");
  }
  if (len >= 0) {
    return w_string(&result[0], len);
  }

  throw std::system_error(errno, std::generic_category(),
                          "readlink for getOpenedPath");
#else
  throw std::system_error(ENOSYS, std::generic_category(),
                          "getOpenedPath not implemented on this platform");
#endif
}

#ifndef _WIN32
w_string FileDescriptor::readSymbolicLink() const {
  struct stat st;
  if (fstat(fd_, &st)) {
    throw std::system_error(
        errno, std::generic_category(), "fstat for readSymbolicLink");
  }
  std::string result;
  result.resize(st.st_size + 1, 0);

#ifdef __linux__
  // Linux 2.6.39 and later provide this interface
  auto atlen = readlinkat(fd_, "", &result[0], result.size());
  if (atlen == int(result.size())) {
    // It's longer than we expected; TOCTOU detected!
    throw std::system_error(
        ENAMETOOLONG, std::generic_category(),
        "readlinkat: link contents grew while examining file");
  }
  if (atlen >= 0) {
    return w_string(result.data(), atlen);
  }
  // if we get ENOTDIR back then we're probably on an older linux and
  // should fall back to the technique used below.
  if (errno != ENOTDIR) {
    throw std::system_error(
        errno, std::generic_category(), "readlinkat for readSymbolicLink");
  }
#endif

  auto myName = getOpenedPath();
  auto len = readlink(myName.c_str(), &result[0], result.size());
  if (len == int(result.size())) {
    // It's longer than we expected; TOCTOU detected!
    throw std::system_error(
        ENAMETOOLONG, std::generic_category(),
        "readlink: link contents grew while examining file");
  }
  if (len >= 0) {
    return w_string(result.data(), len);
  }

  throw std::system_error(
      errno, std::generic_category(), "readlink for readSymbolicLink");
}
#endif

FileInformation getFileInformation(const char *path,
                                   CaseSensitivity caseSensitive) {
  auto options = OpenFileHandleOptions::queryFileInfo();
  options.caseSensitive = caseSensitive;
#if defined(_WIN32) || defined(O_PATH)
  // These operating systems allow opening symlink nodes and querying them
  // for stat information
  auto handle = openFileHandle(path, options);
  auto info = handle.getInfo();
  return info;
#else
  // Since the leaf of the path may be a symlink, and this system doesn't
  // allow opening symlinks for stat purposes, we have to resort to performing
  // a relative fstatat() from the parent dir.
  w_string_piece pathPiece(path);
  auto parent = pathPiece.dirName().asWString();
  auto handle = openFileHandle(parent.c_str(), options);
  struct stat st;
  if (fstatat(
        handle.fd(), pathPiece.baseName().data(), &st, AT_SYMLINK_NOFOLLOW)) {
    throw std::system_error(errno, std::generic_category(), "fstatat");
  }

  if (caseSensitive == CaseSensitivity::Unknown) {
    caseSensitive = getCaseSensitivityForPath(path);
  }
  if (caseSensitive == CaseSensitivity::CaseInSensitive) {
    // We need to perform one extra check for case-insensitive
    // paths to make sure that we didn't accidentally open
    // the wrong case name.
    checkCanonicalBaseName(path);
  }

  return FileInformation(st);
#endif
}

CaseSensitivity getCaseSensitivityForPath(const char *path) {
#ifdef __APPLE__
  return pathconf(path, _PC_CASE_SENSITIVE) ? CaseSensitivity::CaseSensitive
                                            : CaseSensitivity::CaseInSensitive;
#elif defined(_WIN32)
  unused_parameter(path);
  return CaseSensitivity::CaseInSensitive;
#else
  unused_parameter(path);
  return CaseSensitivity::CaseSensitive;
#endif

}

}
