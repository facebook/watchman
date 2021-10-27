/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <map>
#include "watchman/fs/FileSystem.h"

namespace watchman {

class FakeDirHandle : public DirHandle {
 public:
  const DirEntry* readDir() override {
    return nullptr;
  }

#ifndef _WIN32
  int getFd() const override {
    return 0;
  }
#endif
};

class FakeFileSystem : public FileSystem {
 public:
  static constexpr uid_t kDefaultUid = 1001;
  static constexpr gid_t kDefaultGid = 1002;
  static constexpr dev_t kDefaultDev = 1;

  std::unique_ptr<DirHandle> openDir(const char* path, bool strict = true)
      override;

  FileInformation getFileInformation(
      const char* path,
      CaseSensitivity caseSensitive = CaseSensitivity::Unknown) override;

  void addDir(std::string path, const FileInformation& fi);

  FileInformation fakeDir();

 private:
  std::atomic<ino_t> inodeNumber_{1};
  std::map<std::string, FileInformation> byPath_;
};

} // namespace watchman
