/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/InMemoryView.h"
#include <folly/portability/GTest.h>
#include "watchman/fs/FSDetect.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/root/Root.h"
#include "watchman/test/lib/FakeFileSystem.h"
#include "watchman/test/lib/FakeWatcher.h"
#include "watchman/watcher/Watcher.h"

namespace {

using namespace watchman;

class InMemoryViewTest : public testing::Test {
 public:
  using Continue = InMemoryView::Continue;

  const w_string root_path{"/root"};

  FakeFileSystem fs;
  Configuration config;
  std::shared_ptr<FakeWatcher> watcher = std::make_shared<FakeWatcher>(fs);
  PendingCollection pending;

  std::shared_ptr<InMemoryView> view =
      std::make_shared<InMemoryView>(fs, root_path, config, watcher);

  InMemoryViewTest() {
    pending.lock()->ping();
  }
};

TEST_F(InMemoryViewTest, can_construct) {
  fs.defineContents({
      "/root",
  });

  Root root{
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {}};
}

TEST_F(InMemoryViewTest, drive_initial_crawl) {
  fs.defineContents({"/root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};

  // This will perform the initial crawl.
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx{&query, root, false};
  view->pathGenerator(&query, &ctx);

  EXPECT_EQ(2, ctx.resultsArray.size());
  EXPECT_STREQ("dir", ctx.resultsArray.at(0).asCString());
  EXPECT_STREQ("dir/file.txt", ctx.resultsArray.at(1).asCString());
}

TEST_F(InMemoryViewTest, respond_to_watcher_events) {
  getLog().setStdErrLoggingLevel(DBG);

  fs.defineContents({"/root/dir/file.txt"});

  auto root = std::make_shared<Root>(
      fs, root_path, "fs_type", w_string_to_json("{}"), config, view, [] {});

  InMemoryView::IoThreadState state{std::chrono::minutes(5)};
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  Query query;
  query.fieldList.add("name");
  query.fieldList.add("size");
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx1{&query, root, false};
  view->pathGenerator(&query, &ctx1);

  EXPECT_EQ(2, ctx1.resultsArray.size());

  auto one = ctx1.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  auto two = ctx1.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Update filesystem and ensure the query results don't update.

  fs.updateMetadata(
      "/root/dir/file.txt", [&](FileInformation& fi) { fi.size = 100; });
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx2{&query, root, false};
  view->pathGenerator(&query, &ctx2);

  one = ctx2.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx2.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(0, two.get("size").asInt());

  // Now notify the iothread of the change, process events, and assert the view
  // updates.
  pending.lock()->add("/root/dir/file.txt", {}, W_PENDING_VIA_NOTIFY);
  pending.lock()->ping();
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state, pending));

  QueryContext ctx3{&query, root, false};
  view->pathGenerator(&query, &ctx3);

  one = ctx3.resultsArray.at(0);
  EXPECT_STREQ("dir", one.get("name").asCString());
  EXPECT_EQ(0, one.get("size").asInt());
  two = ctx3.resultsArray.at(1);
  EXPECT_STREQ("dir/file.txt", two.get("name").asCString());
  EXPECT_EQ(100, two.get("size").asInt());
}

} // namespace
