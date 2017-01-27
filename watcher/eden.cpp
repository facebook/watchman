/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include "eden/fs/service/gen-cpp2/EdenService.h"
#include "watchman.h"

#include "QueryableView.h"

using facebook::eden::EdenServiceAsyncClient;
using facebook::eden::FileInformationOrError;

namespace watchman {
namespace {

folly::SocketAddress getEdenServerSocketAddress() {
  folly::SocketAddress addr;
  // In the future, eden will need to provide a well-supported way to locate
  // this socket, as the "local" path component here is a FB-specific default.
  auto path = folly::to<std::string>(getenv("HOME"), "/local/.eden/socket");
  addr.setFromPath(path);
  return addr;
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<EdenServiceAsyncClient> getEdenClient(
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  return std::make_unique<EdenServiceAsyncClient>(
      apache::thrift::HeaderClientChannel::newChannel(
          apache::thrift::async::TAsyncSocket::newSocket(
              eb, getEdenServerSocketAddress())));
}

// This is an ugly shim that allows creating watchman_dir and
// watchman_file objects to satisfy eden queries.
class FakeDirs {
 public:
  explicit FakeDirs(const w_string& root_path)
      : root_(root_path, nullptr), root_path_(root_path) {}

  // Similar to InMemoryView::resolveDir with create=true
  watchman_dir* resolveDir(w_string_piece dir_name) {
    if (dir_name.size() == 0) {
      return &root_;
    }

    auto dir = &root_;
    watchman_dir* parent = nullptr;

    while (true) {
      auto sep = (const char*)memchr(dir_name.data(), '/', dir_name.size());
      uint32_t child_name_size = sep ? sep - dir_name.data() : dir_name.size();
      w_string_t component;
      w_string_new_len_typed_stack(
          &component, dir_name.data(), child_name_size, W_STRING_BYTE);

      auto child = dir->getChildDir(&component);
      if (!child) {
        // We need to create the directory
        w_string child_name(dir_name.data(), child_name_size);
        auto& new_child = dir->dirs[child_name];
        new_child.reset(new watchman_dir(child_name, dir));
        child = new_child.get();
      }

      parent = dir;
      dir = child;
      if (!sep) {
        return dir;
      }
      dir_name.advance((sep - dir_name.data()) + 1);
    }
    // Not reached
  }

  std::unique_ptr<watchman_file, watchman_dir::Deleter> makeFile(
      w_string_piece name,
      const FileInformationOrError& info) {
    auto dir = resolveDir(name.dirName());
    auto file = watchman_file::make(name.baseName().asWString(), dir);

    if (info.getType() == FileInformationOrError::info) {
      file->stat.size = info.get_info().size;
      file->stat.mode = info.get_info().mode;
      file->stat.mtime.tv_sec = info.get_info().mtime.seconds;
      file->stat.mtime.tv_nsec = info.get_info().mtime.nanoSeconds;
    } else {
      file->exists = false;
    }

    return file;
  }

 private:
  watchman_dir root_;
  w_string root_path_;
};

class EdenView : public QueryableView {
  w_string root_path_;

 public:
  explicit EdenView(w_root_t* root) {
    root_path_ = root->root_path;
  }

  bool timeGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  bool syncToNow(std::chrono::milliseconds) override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return true;
  }

  /** Walks files that match the supplied set of paths */
  bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    bool result = true;
    watchman::log(watchman::ERR, __FUNCTION__, "\n");

    auto client = getEdenClient();
    std::vector<std::string> fileNames;

    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    std::string globPattern;
    if (query->relative_root) {
      w_string_piece rel(query->relative_root);
      rel.advance(ctx->root->root_path.size() + 1);
      globPattern.append(rel.data(), rel.size());
      globPattern.append("/");
    }
    globPattern.append("**");

    std::string mountPoint(
        ctx->root->root_path.data(), ctx->root->root_path.size());

    client->sync_glob(
        fileNames, mountPoint, std::vector<std::string>{globPattern});

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    FakeDirs dirs(ctx->root->root_path);

    if (info.size() != fileNames.size()) {
      throw std::runtime_error(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = dirs.makeFile(name, fileInfo);

      if (!w_query_process_file(ctx->query, ctx, file.get())) {
        result = false;
        break;
      }

      ++nameIter;
      ++infoIter;
    }

    *num_walked = fileNames.size();
    return result;
  }

  ClockPosition getMostRecentRootNumberAndTickValue() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return ClockPosition();
  }

  w_string getCurrentClockString() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return ClockPosition().toClockString();
  }

  uint32_t getLastAgeOutTickValue() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return 0;
  }

  time_t getLastAgeOutTimeStamp() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return 0;
  }

  void ageOut(w_perf_t& sample, std::chrono::seconds minAge) override {}

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  void startThreads(const std::shared_ptr<w_root_t>& root) override {}
  void signalThreads() override {}

  const w_string& getName() const override {
    static w_string name("eden");
    return name;
  }

  std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<w_root_t>& root) override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }
};

std::shared_ptr<watchman::QueryableView> detectEden(w_root_t* root) {
  // This is mildly ghetto, but the way we figure out if the intended path
  // is on an eden mount is to ask eden to stat the root of that mount;
  // if it throws then it is not an eden mount.
  auto client = getEdenClient();

  std::vector<FileInformationOrError> info;
  static const std::vector<std::string> paths{""};
  client->sync_getFileInformation(
      info, std::string(root->root_path.data(), root->root_path.size()), paths);

  return std::make_shared<EdenView>(root);
}

} // anon namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // watchman namespace
