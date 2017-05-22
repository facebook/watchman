/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#ifdef _WIN32
#include "Win32Handle.h"
#include "watchman.h"
#include "FileSystem.h"

namespace watchman {

Win32Handle::~Win32Handle() {
  close();
}

Win32Handle::Win32Handle(intptr_t h) : h_(h) {
  // Normalize to a single invalid value for validity checks
  if (h_ == intptr_t(INVALID_HANDLE_VALUE)) {
    h_ = 0;
  }
}

Win32Handle::Win32Handle(Win32Handle&& other) noexcept : h_(other.release()) {}

Win32Handle& Win32Handle::operator=(Win32Handle&& other) noexcept {
  close();
  h_ = other.h_;
  other.h_ = 0;
  return *this;
}

void Win32Handle::close() {
  if (h_) {
    CloseHandle((HANDLE)h_);
    h_ = 0;
  }
}

intptr_t Win32Handle::release() {
  intptr_t res = h_;
  h_ = 0;
  return res;
}

Win32Handle openFileHandle(const char *path,
                           const OpenFileHandleOptions &opts) {
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

  attrs = FILE_FLAG_POSIX_SEMANTICS;
  if (opts.metaDataOnly) {
    attrs |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  if (!opts.followSymlinks) {
    attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
  }

  Win32Handle h(intptr_t(
      CreateFileW(wpath.c_str(), access, share, &sec, create, attrs, nullptr)));
  err = GetLastError();

  if (!h) {
    throw std::system_error(err, std::system_category(),
                            std::string("CreateFileW for openFileHandle: ") +
                                path);
  }
  return h;
}

FileInformation Win32Handle::getInfo() const {
  FILE_BASIC_INFO binfo;
  FILE_STANDARD_INFO sinfo;

  if (!GetFileInformationByHandleEx((HANDLE)handle(), FileBasicInfo, &binfo,
                                    sizeof(binfo))) {
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
}

}
#endif // _WIN32
