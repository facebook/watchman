/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "FileDescriptor.h"
#include "FileSystem.h"
#include "watchman.h"
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/utsname.h>
#include <sys/vnode.h>
#endif
#include <system_error>
#ifdef _WIN32
#include "WinIoCtl.h"
#endif
#include "watchman_scopeguard.h"

#if defined(_WIN32) || defined(O_PATH)
#define CAN_OPEN_SYMLINKS 1
#else
#define CAN_OPEN_SYMLINKS 0
#endif

#ifdef _WIN32
// We declare our own copy here because Ntifs.h is not included in the
// standard install of the Visual Studio Community compiler.
namespace {
struct REPARSE_DATA_BUFFER {
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  union {
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      ULONG Flags;
      WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  };
};
}
#endif

namespace watchman {

FileDescriptor::~FileDescriptor() {
  close();
}

FileDescriptor::system_handle_type FileDescriptor::normalizeHandleValue(
    system_handle_type h) {
#ifdef _WIN32
  // Windows uses both 0 and INVALID_HANDLE_VALUE as invalid handle values.
  if (h == intptr_t(INVALID_HANDLE_VALUE) || h == 0) {
    return FileDescriptor::kInvalid;
  }
#else
  // Posix defines -1 to be an invalid value, but we'll also recognize and
  // normalize any negative descriptor value.
  if (h < 0) {
    return FileDescriptor::kInvalid;
  }
#endif
  return h;
}

FileDescriptor::FileDescriptor(FileDescriptor::system_handle_type fd)
    : fd_(normalizeHandleValue(fd)) {}

FileDescriptor::FileDescriptor(
    FileDescriptor::system_handle_type fd,
    const char* operation)
    : fd_(normalizeHandleValue(fd)) {
  if (fd_ == kInvalid) {
    throw std::system_error(
        errno,
        std::generic_category(),
        std::string(operation) + ": " + strerror(errno));
  }
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  close();
  fd_ = other.fd_;
  other.fd_ = kInvalid;
  return *this;
}

void FileDescriptor::close() {
  if (fd_ != kInvalid) {
#ifndef _WIN32
    ::close(fd_);
#else
    CloseHandle((HANDLE)fd_);
#endif
    fd_ = kInvalid;
  }
}

FileDescriptor::system_handle_type FileDescriptor::release() {
  system_handle_type result = fd_;
  fd_ = kInvalid;
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

FileDescriptor openFileHandle(const char *path,
                              const OpenFileHandleOptions &opts) {
#ifndef _WIN32
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
#else // _WIN32
  DWORD access = 0, share = 0, create = 0, attrs = 0;
  DWORD err;
  SECURITY_ATTRIBUTES sec;

  if (!strcmp(path, "/dev/null")) {
    path = "NUL:";
  }

  auto wpath = w_string_piece(path).asWideUNC();

  if (opts.metaDataOnly) {
    access = 0;
  } else {
    if (opts.writeContents) {
      access |= GENERIC_WRITE;
    }
    if (opts.readContents) {
      access |= GENERIC_READ;
    }
  }

  // We want more posix-y behavior by default
  share = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;

  memset(&sec, 0, sizeof(sec));
  sec.nLength = sizeof(sec);
  sec.bInheritHandle = TRUE;
  if (opts.closeOnExec) {
    sec.bInheritHandle = FALSE;
  }

  if (opts.create && opts.exclusiveCreate) {
    create = CREATE_NEW;
  } else if (opts.create && opts.truncate) {
    create = CREATE_ALWAYS;
  } else if (opts.create) {
    create = OPEN_ALWAYS;
  } else if (opts.truncate) {
    create = TRUNCATE_EXISTING;
  } else {
    create = OPEN_EXISTING;
  }

  attrs = FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_BACKUP_SEMANTICS;
  if (!opts.followSymlinks) {
    attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
  }

  FileDescriptor file(intptr_t(
      CreateFileW(wpath.c_str(), access, share, &sec, create, attrs, nullptr)));
  err = GetLastError();
  if (!file) {
    throw std::system_error(
        err,
        std::system_category(),
        std::string("CreateFileW for openFileHandle: ") + path);
  }

#endif

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
#ifndef _WIN32
  struct stat st;
  if (fstat(fd_, &st)) {
    int err = errno;
    throw std::system_error(err, std::generic_category(), "fstat");
  }
  return FileInformation(st);
#else // _WIN32
  FILE_BASIC_INFO binfo;
  FILE_STANDARD_INFO sinfo;

  if (!GetFileInformationByHandleEx(
          (HANDLE)handle(), FileBasicInfo, &binfo, sizeof(binfo))) {
    throw std::system_error(
        GetLastError(),
        std::system_category(),
        "GetFileInformationByHandleEx FileBasicInfo");
  }

  FileInformation info(binfo.FileAttributes);

  FILETIME_LARGE_INTEGER_to_timespec(binfo.CreationTime, &info.ctime);
  FILETIME_LARGE_INTEGER_to_timespec(binfo.LastAccessTime, &info.atime);
  FILETIME_LARGE_INTEGER_to_timespec(binfo.LastWriteTime, &info.mtime);

  if (!GetFileInformationByHandleEx(
          (HANDLE)handle(), FileStandardInfo, &sinfo, sizeof(sinfo))) {
    throw std::system_error(
        GetLastError(),
        std::system_category(),
        "GetFileInformationByHandleEx FileStandardInfo");
  }

  info.size = sinfo.EndOfFile.QuadPart;
  info.nlink = sinfo.NumberOfLinks;

  return info;
#endif
}


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
#elif defined(_WIN32)
  std::wstring wchar;
  wchar.resize(WATCHMAN_NAME_MAX);
  auto len = GetFinalPathNameByHandleW(
      (HANDLE)fd_,
      &wchar[0],
      wchar.size(),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  auto err = GetLastError();

  if (len >= wchar.size()) {
    // Grow it
    wchar.resize(len);
    len = GetFinalPathNameByHandleW(
        (HANDLE)fd_, &wchar[0], len, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    err = GetLastError();
  }

  if (len == 0) {
    throw std::system_error(
        GetLastError(), std::system_category(), "GetFinalPathNameByHandleW");
  }

  return w_string(wchar.data(), len);
#else
  throw std::system_error(ENOSYS, std::generic_category(),
                          "getOpenedPath not implemented on this platform");
#endif
}

w_string readSymbolicLink(const char* path) {
#ifndef _WIN32
  std::string result;

  // Speculatively assume that this is large enough to read the
  // symlink text.  This helps to avoid an extra lstat call.
  result.resize(256);

  for (int retry = 0; retry < 2; ++retry) {
    auto len = readlink(path, &result[0], result.size());
    if (len < 0) {
      throw std::system_error(
          errno, std::generic_category(), "readlink for readSymbolicLink");
    }
    if (size_t(len) < result.size()) {
      return w_string(result.data(), len);
    }

    // Truncated read; we need to figure out the right size to use
    struct stat st;
    if (lstat(path, &st)) {
      throw std::system_error(
          errno, std::generic_category(), "lstat for readSymbolicLink");
    }

    result.resize(st.st_size + 1, 0);
  }

  throw std::system_error(
      E2BIG,
      std::generic_category(),
      "readlink for readSymbolicLink: symlink changed while reading it");
#else
  return openFileHandle(path, OpenFileHandleOptions::queryFileInfo())
      .readSymbolicLink();
#endif
}

w_string FileDescriptor::readSymbolicLink() const {
#ifndef _WIN32
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
#else // _WIN32
  DWORD len = 64 * 1024;
  auto buf = malloc(len);
  if (!buf) {
    throw std::bad_alloc();
  }
  SCOPE_EXIT {
    free(buf);
  };
  WCHAR* target;
  USHORT targetlen;

  auto result = DeviceIoControl(
      (HANDLE)fd_,
      FSCTL_GET_REPARSE_POINT,
      nullptr,
      0,
      buf,
      len,
      &len,
      nullptr);

  // We only give one retry; if the size changed again already, we'll
  // have another pending notify from the OS to go look at it again
  // later, and it's totally fine to give up here for now.
  if (!result && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    free(buf);
    buf = malloc(len);
    if (!buf) {
      throw std::bad_alloc();
    }

    result = DeviceIoControl(
        (HANDLE)fd_,
        FSCTL_GET_REPARSE_POINT,
        nullptr,
        0,
        buf,
        len,
        &len,
        nullptr);
  }

  if (!result) {
    throw std::system_error(
        GetLastError(), std::system_category(), "FSCTL_GET_REPARSE_POINT");
  }

  auto rep = reinterpret_cast<REPARSE_DATA_BUFFER*>(buf);

  switch (rep->ReparseTag) {
    case IO_REPARSE_TAG_SYMLINK:
      target = rep->SymbolicLinkReparseBuffer.PathBuffer +
          (rep->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
      targetlen =
          rep->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
      break;

    case IO_REPARSE_TAG_MOUNT_POINT:
      target = rep->MountPointReparseBuffer.PathBuffer +
          (rep->MountPointReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
      targetlen =
          rep->MountPointReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
      break;
    default:
      throw std::system_error(
          ENOSYS, std::generic_category(), "Unsupported ReparseTag");
  }

  return w_string(target, targetlen);
#endif
}

w_string realPath(const char *path) {
  auto options = OpenFileHandleOptions::queryFileInfo();
  // Follow symlinks, because that's really the point of this function
  options.followSymlinks = 1;
  options.strictNameChecks = 0;

#ifdef _WIN32
  // Special cases for cwd
  w_string_piece pathPiece(path);
  // On Windows, "" is used to refer to the CWD.
  // We also allow using "." for parity with unix, even though that
  // doesn't generally work for that purpose on windows.
  // This allows `watchman watch-project .` to succeeed on windows.
  if (pathPiece.size() == 0 || pathPiece == ".") {
    std::wstring wchar;
    wchar.resize(WATCHMAN_NAME_MAX);
    auto len = GetCurrentDirectoryW(wchar.size(), &wchar[0]);
    auto err = GetLastError();
    if (len == 0) {
      throw std::system_error(err, std::system_category(),
                              "GetCurrentDirectoryW");
    }
    // Assumption: that the OS maintains the CWD in canonical form
    return w_string(wchar.data(), len);
  }
#endif

  auto handle = openFileHandle(path, options);
  return handle.getOpenedPath();
}

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

Result<int, std::error_code> FileDescriptor::read(void* buf, int size) const {
#ifndef _WIN32
  auto result = ::read(fd_, buf, size);
  if (result == -1) {
    int errcode = errno;
    return Result<int, std::error_code>(
        std::error_code(errcode, std::generic_category()));
  }
  return Result<int, std::error_code>(result);
#else
  DWORD result = 0;
  if (!ReadFile((HANDLE)fd_, buf, size, &result, nullptr)) {
    return Result<int, std::error_code>(
        std::error_code(GetLastError(), std::system_category()));
  }
  return Result<int, std::error_code>(result);
#endif
}

Result<int, std::error_code> FileDescriptor::write(const void* buf, int size)
    const {
#ifndef _WIN32
  auto result = ::write(fd_, buf, size);
  if (result == -1) {
    int errcode = errno;
    return Result<int, std::error_code>(
        std::error_code(errcode, std::generic_category()));
  }
  return Result<int, std::error_code>(result);
#else
  DWORD result = 0;
  if (!WriteFile((HANDLE)fd_, buf, size, &result, nullptr)) {
    return Result<int, std::error_code>(
        std::error_code(GetLastError(), std::system_category()));
  }
  return Result<int, std::error_code>(result);
#endif
}

const FileDescriptor& FileDescriptor::stdIn() {
  static FileDescriptor f(
#ifdef _WIN32
      intptr_t(GetStdHandle(STD_INPUT_HANDLE))
#else
      STDIN_FILENO
#endif
          );
  return f;
}

const FileDescriptor& FileDescriptor::stdOut() {
  static FileDescriptor f(
#ifdef _WIN32
      intptr_t(GetStdHandle(STD_OUTPUT_HANDLE))
#else
      STDOUT_FILENO
#endif
          );
  return f;
}

const FileDescriptor& FileDescriptor::stdErr() {
  static FileDescriptor f(
#ifdef _WIN32
      intptr_t(GetStdHandle(STD_ERROR_HANDLE))
#else
      STDERR_FILENO
#endif
          );
  return f;
}
}
