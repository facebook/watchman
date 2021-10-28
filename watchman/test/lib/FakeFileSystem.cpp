/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/test/lib/FakeFileSystem.h"
#include <folly/MapUtil.h>

namespace watchman {

namespace {

folly::StringPiece ensureAbsolute(folly::StringPiece path) {
  if (path.removePrefix('/')) {
    return path;
  } else {
    throw std::logic_error{fmt::format("Path {} must be absolute", path)};
  }
}

folly::StringPiece ensureAbsolute(const char* path) {
  return ensureAbsolute(folly::StringPiece{path});
}

template <typename Func>
std::invoke_result_t<Func, const FakeInode&>
withPath(const FakeInode& root, folly::StringPiece path, Func&& func) {
  auto piece = ensureAbsolute(path);

  const FakeInode* inode = &root;
  while (!piece.empty()) {
    size_t idx = piece.find('/');
    folly::StringPiece this_level;
    if (idx == folly::StringPiece::npos) {
      this_level = piece;
      piece.clear();
    } else {
      this_level = piece.subpiece(0, idx);
      piece.advance(idx + 1);
    }

    inode = folly::get_ptr(inode->children, this_level.str());
    if (!inode) {
      throw std::system_error(
          ENOENT, std::generic_category(), fmt::format("no file at {}", path));
    }
  }

  return func(*inode);
}

class FakeDirHandle : public DirHandle {
 public:
  struct FakeDirEntry {
    std::string name;
    std::optional<FileInformation> stat;
  };

  explicit FakeDirHandle(std::vector<FakeDirEntry> entries)
      : entries_{std::move(entries)} {}

  const DirEntry* readDir() override {
    if (idx_ >= entries_.size()) {
      return nullptr;
    }

    auto& e = entries_[idx_++];
    current_.has_stat = e.stat.has_value();
    current_.d_name = e.name.c_str();
    current_.stat = e.stat ? e.stat.value() : FileInformation{};
    return &current_;
  }

#ifndef _WIN32
  int getFd() const override {
    return 0;
  }
#endif

 private:
  size_t idx_ = 0;
  DirEntry current_;
  std::vector<FakeDirEntry> entries_;
};

} // namespace

FakeFileSystem::Flags::Flags() = default;

FakeFileSystem::FakeFileSystem(Flags flags)
    : flags_{flags}, root_{folly::in_place, FakeInode{fakeDir()}} {}

std::unique_ptr<DirHandle> FakeFileSystem::openDir(
    const char* path,
    bool strict) {
  auto root = root_.rlock();
  return withPath(*root, path, [&](const FakeInode& inode) {
    // TODO: assert it's a directory

    // TODO: implement strict case checking
    (void)strict;
    std::vector<FakeDirHandle::FakeDirEntry> entries;
    for (auto& [name, child] : inode.children) {
      FakeDirHandle::FakeDirEntry entry;
      entry.name = name;
      if (flags_.includeReadDirStat) {
        entry.stat = child.metadata;
      }
      entries.push_back(std::move(entry));
    }

    return std::make_unique<FakeDirHandle>(std::move(entries));
  });
}

FileInformation FakeFileSystem::getFileInformation(
    const char* path,
    CaseSensitivity caseSensitive) {
  auto root = root_.rlock();
  return withPath(*root, path, [&](const FakeInode& inode) {
    // TODO: validate case
    (void)caseSensitive;

    return inode.metadata;
  });
}

void FakeFileSystem::defineContents(std::initializer_list<const char*> paths) {
  for (folly::StringPiece path : paths) {
    if (path.removeSuffix('/')) {
      addNode(path.str().c_str(), fakeDir());
    } else {
      addNode(path.str().c_str(), fakeFile());
    }
  }
}

void FakeFileSystem::addNode(const char* path, const FileInformation& fi) {
  auto root = root_.wlock();
  FakeInode* inode = &*root;

  auto piece = ensureAbsolute(path);
  while (!piece.empty()) {
    size_t idx = piece.find('/');
    folly::StringPiece this_level;
    if (idx == folly::StringPiece::npos) {
      this_level = piece;
      piece.clear();
    } else {
      this_level = piece.subpiece(0, idx);
      piece.advance(idx + 1);
    }

    FakeInode* child = folly::get_ptr(inode->children, this_level.str());
    if (!child) {
      // TODO: add option for whether autocreate is desired or not
      FakeInode fakeInode{fakeDir()};
      auto [iter, yes] = inode->children.emplace(this_level.str(), fakeInode);
      child = &iter->second;
    } else {
      // TODO: ensure child is a directory
    }
    inode = child;
  }

  inode->metadata = fi;
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

FileInformation FakeFileSystem::fakeFile() {
  FileInformation fi{};
  fi.mode = S_IFREG;
  fi.size = 0;
  fi.uid = kDefaultUid;
  fi.gid = kDefaultGid;
  fi.ino = inodeNumber_.fetch_add(1, std::memory_order_acq_rel);
  fi.dev = kDefaultDev;
  fi.nlink = 1;
  // fi.fileAttributes = 0;
  // TODO: populate timestamps
  return fi;
}

} // namespace watchman
