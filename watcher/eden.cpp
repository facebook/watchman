/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include "eden/fs/service/gen-cpp2/EdenService.h"
#include "watchman.h"

#include "QueryableView.h"

using facebook::eden::EdenServiceAsyncClient;

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
    return false;
  }

  bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    return false;
  }

  /** Walks files that match the supplied set of paths */
  bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    return false;
  }

  bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    return false;
  }

  bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    return false;
  }

  ClockPosition getMostRecentRootNumberAndTickValue() const override {
    return ClockPosition();
  }

  w_string getCurrentClockString() const override {
    return ClockPosition().toClockString();
  }

  uint32_t getLastAgeOutTickValue() const override {
    return 0;
  }

  time_t getLastAgeOutTimeStamp() const override {
    return 0;
  }

  void ageOut(w_perf_t& sample, std::chrono::seconds minAge) override {}

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const override {
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

  std::vector<::facebook::eden::FileInformationOrError> info;
  static const std::vector<std::string> paths{""};
  client->sync_getFileInformation(
      info, std::string(root->root_path.data(), root->root_path.size()), paths);

  return std::make_shared<EdenView>(root);
}

} // anon namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // watchman namespace
