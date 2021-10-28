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

  std::shared_ptr<InMemoryView> view =
      std::make_shared<InMemoryView>(fs, root_path, config, watcher);
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
  EXPECT_EQ(Continue::Continue, view->stepIoThread(root, state));

  Query query;
  parse_field_list(json_array({w_string_to_json("name")}), &query.fieldList);
  query.paths.emplace();
  query.paths->emplace_back(QueryPath{"", 1});

  QueryContext ctx{&query, root, false};
  view->pathGenerator(&query, &ctx);

  EXPECT_EQ(2, json_array_size(ctx.resultsArray));
  EXPECT_STREQ("dir", json_string_value(ctx.resultsArray.at(0)));
  EXPECT_STREQ("dir/file.txt", json_string_value(ctx.resultsArray.at(1)));
}

} // namespace
