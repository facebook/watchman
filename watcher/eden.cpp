/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "QueryableView.h"

namespace watchman {
namespace {
class EdenView : public QueryableView {
 public:
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
  throw std::runtime_error("not an eden mount");
}

} // anon namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // watchman namespace
