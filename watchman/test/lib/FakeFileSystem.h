/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Synchronized.h>
#include <atomic>
#include <map>
#include "watchman/fs/FileSystem.h"

namespace watchman {

struct FakeInode {
  FileInformation metadata;
  // For testability, a defined order is useful. Lexicographical ordering is
  // fine, though it might be nice to support arbitrary orders in the future.
  // After all, operating systems don't guarantee any particular order from
  // readdir.
  std::map<std::string, FakeInode> children;

  explicit FakeInode(const FileInformation& fi) : metadata{fi} {}
};

class FakeFileSystem : public FileSystem {
 public:
  static constexpr uid_t kDefaultUid = 1001;
  static constexpr gid_t kDefaultGid = 1002;
  static constexpr dev_t kDefaultDev = 1;

  struct Flags {
    // For the FakeFileSystem constructor below, this constructor must be
    // defined externally.
    Flags();

    // Default to POSIX semantics. Set true for readdirplus / Windows semantics.
    bool includeReadDirStat = false;
  };

  explicit FakeFileSystem(Flags flags = Flags{});

  std::unique_ptr<DirHandle> openDir(const char* path, bool strict = true)
      override;

  FileInformation getFileInformation(
      const char* path,
      CaseSensitivity caseSensitive = CaseSensitivity::Unknown) override;

  void defineContents(std::initializer_list<const char*> paths);

  void addNode(const char* path, const FileInformation& fi);

  FileInformation fakeDir();
  FileInformation fakeFile();

 private:
  const Flags flags_;
  std::atomic<ino_t> inodeNumber_{1};
  folly::Synchronized<FakeInode> root_;
};

} // namespace watchman
