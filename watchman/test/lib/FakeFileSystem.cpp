/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/test/lib/FakeFileSystem.h"
#include <folly/MapUtil.h>

namespace watchman {

std::unique_ptr<DirHandle> FakeFileSystem::openDir(
    const char* path,
    bool strict) {
  (void)path;
  (void)strict;
  return std::make_unique<FakeDirHandle>();
}

FileInformation FakeFileSystem::getFileInformation(
    const char* path,
    CaseSensitivity caseSensitive) {
  (void)caseSensitive;
  if (const FileInformation* fi = folly::get_ptr(byPath_, path)) {
    // TODO: validate case sensitivity
    return *fi;
  } else {
    throw std::system_error(
        ENOENT, std::generic_category(), fmt::format("no file at {}", path));
  }
}

void FakeFileSystem::addDir(std::string path, const FileInformation& fi) {
  byPath_.emplace(std::move(path), fi);
}

FileInformation FakeFileSystem::fakeDir() {
  FileInformation fi{};
  fi.mode = S_IFDIR;
  fi.size = 0;
  fi.uid = kDefaultUid;
  fi.gid = kDefaultGid;
  fi.ino = inodeNumber_.fetch_add(1, std::memory_order_acq_rel);
  fi.dev = kDefaultDev;
  fi.nlink = 2; // TODO: to populate
  // fi.fileAttributes = 0;
  // TODO: populate timestamps
  return fi;
}

} // namespace watchman
