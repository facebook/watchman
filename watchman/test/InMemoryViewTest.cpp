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
