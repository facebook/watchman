/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/InMemoryView.h"
#include <folly/portability/GTest.h>
#include "watchman/fs/FSDetect.h"
#include "watchman/root/Root.h"
#include "watchman/watcher/Watcher.h"

namespace {

using namespace ::testing;
using namespace watchman;

class FakeWatcher : public Watcher {
 public:
  FakeWatcher() : Watcher{"fake", 0} {}

  std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      struct watchman_dir* dir,
      const char* path) override {
    (void)root;
    (void)dir;
    (void)path;
    throw "boom";
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
};

TEST(InMemoryViewTest, can_construct) {
  Configuration config;
  auto watcher = std::make_shared<FakeWatcher>();

  InMemoryView view{"/fake/root", config, watcher};
}

} // namespace
