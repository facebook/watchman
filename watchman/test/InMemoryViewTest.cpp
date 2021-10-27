/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/InMemoryView.h"
#include <folly/MapUtil.h>
#include <folly/portability/GTest.h>
#include "watchman/fs/FSDetect.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/query/parse.h"
#include "watchman/root/Root.h"
#include "watchman/watcher/Watcher.h"

namespace {

using namespace watchman;

class FakeWatcher : public Watcher {
 public:
  explicit FakeWatcher(FileSystem& fileSystem)
      : Watcher{"fake", 0}, fileSystem_{fileSystem} {}

  std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      struct watchman_dir* dir,
      const char* path) override {
    (void)root;
    (void)dir;
    return fileSystem_.openDir(path);
  }

  bool waitNotify(int timeoutms) override {
    (void)timeoutms;
    throw "boom";
  }

  ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll) override {
    (void)root;
    (void)coll;
    throw "boom";
  }

 private:
  FileSystem& fileSystem_;
};

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
      override {
    (void)path;
    (void)strict;
    return std::make_unique<FakeDirHandle>();
  }

  FileInformation getFileInformation(
      const char* path,
      CaseSensitivity caseSensitive = CaseSensitivity::Unknown) override {
    (void)caseSensitive;
    if (const FileInformation* fi = folly::get_ptr(byPath_, path)) {
      // TODO: validate case sensitivity
      return *fi;
    } else {
      throw std::system_error(
          ENOENT, std::generic_category(), fmt::format("no file at {}", path));
    }
  }

  void addDir(std::string path, const FileInformation& fi) {
    byPath_.emplace(std::move(path), fi);
  }

  FileInformation fakeDir() {
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

 private:
  std::atomic<ino_t> inodeNumber_{1};
  std::map<std::string, FileInformation> byPath_;
};

TEST(InMemoryViewTest, can_construct) {
  FakeFileSystem fs;

  Configuration config;
  auto watcher = std::make_shared<FakeWatcher>(fs);

  w_string root_path{"/fake/root"};
  auto view = std::make_shared<InMemoryView>(fs, root_path, config, watcher);
  Root root{
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {}};
}

TEST(InMemoryViewTest, drive_initial_crawl) {
  using Continue = InMemoryView::Continue;

  FakeFileSystem fs;
  fs.addDir("/fake/root", fs.fakeDir());

  Configuration config;
  auto watcher = std::make_shared<FakeWatcher>(fs);

  w_string root_path{"/fake/root"};
  auto view = std::make_shared<InMemoryView>(fs, root_path, config, watcher);
  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};

  // This will perform the initial crawl.
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state));

  Query query;
  parse_field_list(json_array({w_string_to_json("name")}), &query.fieldList);
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 0});

  QueryContext ctx{&query, root, false};
  view->pathGenerator(&query, &ctx);

  // TODO: assert result set
}

} // namespace
