/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_scopeguard.h"
#include "Win32Handle.h"

using watchman::Win32Handle;

namespace {
class WinDirHandle : public watchman_dir_handle {
  std::wstring dirWPath_;
  Win32Handle h_;
  bool win7_{false};
  FILE_FULL_DIR_INFO* info_{nullptr};
  char __declspec(align(8)) buf_[64 * 1024];
  HANDLE hDirFind_{nullptr};
  char nameBuf_[WATCHMAN_NAME_MAX];
  struct watchman_dir_ent ent_;

  const watchman_dir_ent* readDirWin8() {
    if (!info_) {
      if (!GetFileInformationByHandleEx(
        (HANDLE)h_.handle(), FileFullDirectoryInfo, buf_, sizeof(buf_))) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
          return nullptr;
        }
        throw std::system_error(
          GetLastError(),
          std::system_category(),
          "GetFileInformationByHandleEx");
      }
      info_ = (FILE_FULL_DIR_INFO*)buf_;
    }

    // Decode the item currently pointed at
    DWORD len = WideCharToMultiByte(
      CP_UTF8,
      0,
      info_->FileName,
      info_->FileNameLength / sizeof(WCHAR),
      nameBuf_,
      sizeof(nameBuf_) - 1,
      nullptr,
      nullptr);

    if (len <= 0) {
      throw std::system_error(
        GetLastError(), std::system_category(), "WideCharToMultiByte");
    }

    nameBuf_[len] = 0;

    // Populate stat info to speed up the crawler() routine
    FILETIME_LARGE_INTEGER_to_timespec(info_->CreationTime, &ent_.stat.ctime);
    FILETIME_LARGE_INTEGER_to_timespec(info_->LastAccessTime, &ent_.stat.atime);
    FILETIME_LARGE_INTEGER_to_timespec(info_->LastWriteTime, &ent_.stat.mtime);
    ent_.stat.size = info_->EndOfFile.QuadPart;

    if (info_->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // This is a symlink, but msvcrt has no way to indicate that.
      // We'll treat it as a regular file until we have a better
      // representation :-/
      ent_.stat.mode = _S_IFREG;
    }
    else if (info_->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ent_.stat.mode = _S_IFDIR | S_IEXEC | S_IXGRP | S_IXOTH;
    }
    else {
      ent_.stat.mode = _S_IFREG;
    }
    if (info_->FileAttributes & FILE_ATTRIBUTE_READONLY) {
      ent_.stat.mode |= 0444;
    }
    else {
      ent_.stat.mode |= 0666;
    }

    // Advance the pointer to the next entry ready for the next read
    info_ = info_->NextEntryOffset == 0
      ? nullptr
      : (FILE_FULL_DIR_INFO*)(((char*)info_) + info_->NextEntryOffset);
 
    return &ent_;
  }

  const watchman_dir_ent* readDirWin7() {
    // FileFullDirectoryInfo is not supported prior to Windows 8
    WIN32_FIND_DATAW findFileData;
    if (!hDirFind_) {
      std::wstring strWPath(dirWPath_);
      strWPath += L"\\*";
      if ((hDirFind_ = FindFirstFileW(strWPath.c_str(), &findFileData)) == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
          FindClose(hDirFind_);
          return nullptr;
        }

        throw std::system_error(
          GetLastError(),
          std::system_category(),
          "FindFirstFileW");
      }
    }
    else
    {
      if (!FindNextFileW(hDirFind_, &findFileData)) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
          FindClose(hDirFind_);
          return nullptr;
        }

        throw std::system_error(
          GetLastError(),
          std::system_category(),
          "FindNextFileW");
      }
    }

    DWORD len = WideCharToMultiByte(
      CP_UTF8,
      0,
      findFileData.cFileName,
      -1,
      nameBuf_,
      sizeof(nameBuf_) - 1,
      NULL,
      NULL);

    if (len <= 0) {
      throw std::system_error(
        GetLastError(), std::system_category(), "WideCharToMultiByte");
    }

    nameBuf_[len] = 0;

    // Populate stat info to speed up the crawler() routine
    FILETIME_to_timespec(&findFileData.ftCreationTime, &ent_.stat.ctime);
    FILETIME_to_timespec(&findFileData.ftLastAccessTime, &ent_.stat.atime);
    FILETIME_to_timespec(&findFileData.ftLastWriteTime, &ent_.stat.mtime);

    LARGE_INTEGER fileSize;
    fileSize.HighPart = findFileData.nFileSizeHigh;
    fileSize.LowPart = findFileData.nFileSizeLow;
    ent_.stat.size = fileSize.QuadPart;

    if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      ent_.stat.mode = _S_IFREG;
    }
    else if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ent_.stat.mode = _S_IFDIR | S_IEXEC | S_IXGRP | S_IXOTH;
    }
    else {
      ent_.stat.mode = _S_IFREG;
    }
    if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
      ent_.stat.mode |= 0444;
    }
    else {
      ent_.stat.mode |= 0666;
    }  

    return &ent_;
  }

 public:
  explicit WinDirHandle(const char* path, bool strict) {
    int err = 0;
    dirWPath_ = w_string_piece(path).asWideUNC();

    h_ = Win32Handle(intptr_t(CreateFileW(
        dirWPath_.c_str(),
        GENERIC_READ,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        // Note: FILE_FLAG_OPEN_REPARSE_POINT is equivalent to O_NOFOLLOW,
        // and FILE_FLAG_BACKUP_SEMANTICS is equivalent to O_DIRECTORY
        (strict ? FILE_FLAG_OPEN_REPARSE_POINT : 0) |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)));

    if (!h_) {
      throw std::system_error(
          GetLastError(),
          std::system_category(),
          std::string("CreateFileW for opendir: ") + path);
    }

    // Use Win7 compatibility mode for readDir()
    if (getenv("WATCHMAN_WIN7_COMPAT") &&
        getenv("WATCHMAN_WIN7_COMPAT")[0] == '1') {
      win7_ = true;
    }

    memset(&ent_, 0, sizeof(ent_));
    ent_.d_name = nameBuf_;
    ent_.has_stat = true;
    if (path[1] == ':') {
      ent_.stat.dev = tolower(path[0]) - 'a';
    }
  }

  const watchman_dir_ent* readDir() override {
    if (win7_) {
      return readDirWin7();
    }
    try {
      return readDirWin8();
    }
    catch (const std::system_error& err) {
      // Fallback on Win7 implementation
      win7_ = true;
      return readDirWin7();
    }
  }
};
}

std::unique_ptr<watchman_dir_handle> w_dir_open(const char* path, bool strict) {
  return watchman::make_unique<WinDirHandle>(path, strict);
}
