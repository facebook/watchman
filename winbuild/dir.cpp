/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_scopeguard.h"
#include "Win32Handle.h"

using watchman::Win32Handle;

namespace {
class WinDirHandle : public watchman_dir_handle {
  Win32Handle h_;
  FILE_FULL_DIR_INFO* info_{nullptr};
  char __declspec(align(8)) buf_[64 * 1024];
  char nameBuf_[WATCHMAN_NAME_MAX];
  struct watchman_dir_ent ent_;

 public:
  explicit WinDirHandle(const char* path) {
    int err = 0;
    auto wpath = w_string_piece(path).asWideUNC();

    h_ = Win32Handle(intptr_t(CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        // Note: FILE_FLAG_OPEN_REPARSE_POINT is equivalent to O_NOFOLLOW,
        // and FILE_FLAG_BACKUP_SEMANTICS is equivalent to O_DIRECTORY
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)));

    if (!h_) {
      throw std::system_error(
          GetLastError(),
          std::system_category(),
          std::string("CreateFileW for opendir: ") + path);
    }

    ent_.d_name = nameBuf_;
    ent_.has_stat = false;
  }

  const watchman_dir_ent* readDir() override {
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

    // Advance the pointer to the next entry ready for the next read
    info_ = info_->NextEntryOffset == 0
        ? nullptr
        : (FILE_FULL_DIR_INFO*)(((char*)info_) + info_->NextEntryOffset);
    return &ent_;
  }
};
}

std::unique_ptr<watchman_dir_handle> w_dir_open(const char* path) {
  return watchman::make_unique<WinDirHandle>(path);
}
