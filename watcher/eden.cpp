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
using facebook::eden::FileInformation;

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

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(const FileInformation& info, const w_string& fullName)
      : fullName_(fullName) {
    stat_.size = info.size;
    stat_.mode = info.mode;
    stat_.mtime.tv_sec = info.mtime.seconds;
    stat_.mtime.tv_nsec = info.mtime.nanoSeconds;
  }

  const watchman_stat& stat() const override {
    return stat_;
  }

  w_string_piece baseName() const override {
    return fullName_.piece().baseName();
  }

  w_string_piece dirName() override {
    return fullName_.piece().dirName();
  }

  bool exists() const override {
    return true;
  }

  w_string readLink() const override {
    return nullptr;
  }

  const w_clock_t& ctime() const override {
    throw std::runtime_error("ctime not implemented for eden");
  }
  const w_clock_t& otime() const override {
    throw std::runtime_error("otime not implemented for eden");
  }

 private:
  w_string fullName_;
  watchman_stat stat_;
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
    bool result = true;
    watchman::log(watchman::ERR, __FUNCTION__, "\n");

    auto client = getEdenClient();

    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    w_string_piece rel;
    if (query->relative_root) {
      rel = query->relative_root;
      rel.advance(ctx->root->root_path.size() + 1);
    }

    std::vector<std::string> globStrings;
    // Use the glob array provided by the query_spec.
    // The InMemoryView uses the compiled glob tree but we just want to
    // pass this list through to eden to evaluate.  Note that we're
    // relying on parse_globs() to have already checked that the glob
    // looks sane during query parsing, and that eden itself will
    // sanity check and throw an error if there is still something it
    // doesn't like about it when we call sync_glob() below.
    for (auto& glob : query->query_spec.get("glob").array()) {
      if (query->relative_root) {
        globStrings.emplace_back(
            to<std::string>(rel, "/", json_to_w_string(glob)));
      } else {
        globStrings.emplace_back(to<std::string>(json_to_w_string(glob)));
      }
    }

    // More glob flags/functionality:
    auto noescape = json_is_true(
        query->query_spec.get_default("glob_noescape", json_false()));
    if (noescape) {
      throw std::runtime_error(
          "glob_noescape is not supported for the eden watcher");
    }
    auto includedotfiles = json_is_true(
        query->query_spec.get_default("glob_includedotfiles", json_false()));
    if (includedotfiles) {
      throw std::runtime_error(
          "glob_includedotfiles is not supported for the eden watcher");
    }
    auto mountPoint = to<std::string>(ctx->root->root_path);

    std::vector<std::string> fileNames;
    client->sync_glob(fileNames, mountPoint, globStrings);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo.get_info(), w_string::pathCat({mountPoint, name}));

      if (!w_query_process_file(ctx->query, ctx, std::move(file))) {
        result = false;
        break;
      }

      ++nameIter;
      ++infoIter;
    }

    *num_walked = fileNames.size();
    return result;
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

    if (info.size() != fileNames.size()) {
      throw std::runtime_error(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo.get_info(), w_string::pathCat({mountPoint, name}));

      if (!w_query_process_file(ctx->query, ctx, std::move(file))) {
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
