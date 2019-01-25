/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman_system.h"
#include <folly/executors/ManualExecutor.h>
#include <deque>
#include <string>
#include "LRUCache.h"
#include "thirdparty/tap.h"

using namespace watchman;

static constexpr const std::chrono::milliseconds kErrorTTL(1000);

void test_basics() {
  LRUCache<std::string, bool> cache(5, kErrorTTL);

  ok(cache.size() == 0, "initially empty");
  ok(cache.get("foo") == nullptr, "nullptr for non-existent item");

  ok(cache.set("foo", true)->value(), "inserted true");
  ok(cache.size() == 1, "size is now one");
  ok(cache.get("foo")->value(), "looked up item");

  ok(cache.set("foo", false)->value() == false, "replaced true with false");
  ok(cache.get("foo")->value() == false, "looked up new false item");
  ok(cache.size() == 1, "replacement didn't change size");

  ok(cache.erase("foo")->value() == false, "erased and returned false foo");
  ok(cache.erase("foo") == nullptr, "double erase doesn't return anything");
  ok(cache.get("foo") == nullptr, "nullptr for non-existent item");

  for (size_t i = 0; i < 6; ++i) {
    ok(cache.set(std::to_string(i), true) != nullptr, "inserted");
  }

  ok(cache.size() == 5,
     "limited to 5 items, despite inserting 6 total. size=%" PRIsize_t,
     cache.size());

  ok(cache.get("0") == nullptr, "we expect 0 to have been evicted");
  for (size_t i = 1; i < 6; ++i) {
    ok(cache.get(std::to_string(i)), "found later node %" PRIsize_t, i);
  }

  ok(cache.set("bar", true), "added new item");
  ok(cache.get("1") == nullptr, "we expect 1 to be evicted");
  ok(cache.get("2"), "2 should be there, and we just touched it");
  ok(cache.set("baz", true), "added new item");
  ok(cache.size() == 5, "max size still respected");
  ok(cache.get("2"), "2 should still be there; not evicted");
  ok(cache.get("3") == nullptr, "we expect 3 to be evicted");

  cache.clear();
  ok(cache.size() == 0, "cleared out and have zero items");
}

void test_future() {
  using Cache = LRUCache<int, int>;
  using Node = typename Cache::NodeType;
  Cache cache(5, kErrorTTL);
  folly::ManualExecutor exec;

  auto now = std::chrono::steady_clock::now();

  auto okGetter = [&exec](int k) {
    return folly::makeFuture(k).via(&exec).thenTry(
        [](folly::Try<int>&& key) { return (1 + key.value()) * 2; });
  };

  auto failGetter = [&exec](int k) {
    return folly::makeFuture(k).via(&exec).thenTry(
        [](folly::Try<int> &&) -> int { throw std::runtime_error("bleet"); });
  };

  // Queue up a get via a getter that will succeed
  auto f = cache.get(0, okGetter, now);
  ok(!f.isReady(), "future didn't finish yet");

  try {
    cache.get(0);
    ok(false, "should throw");
  } catch (const std::runtime_error&) {
    ok(true, "should throw runtime_error for mixing getters");
  }

  // Queue up a second get using the same getter
  auto f2 = cache.get(0, okGetter, now);
  ok(!f2.isReady(), "also not ready");

  exec.drain();

  ok(f.isReady(), "first is ready");
  ok(f2.isReady(), "second is ready");

  ok(f.value()->value() == 2, "got correct value for first");
  ok(f.value()->value() == f2.value()->value(), "got same value for second");

  // Now to saturate the cache with failed lookups

  cache.clear();
  std::vector<folly::Future<std::shared_ptr<const Node>>> futures;
  for (size_t i = 1; i < 7; ++i) {
    futures.emplace_back(
        cache.get(i, failGetter, now + std::chrono::milliseconds(i)));
  }

  auto drained = exec.drain();
  ok(drained == 12, "should be 12 things pending, but have %d", drained);

  // Let them make progress
  exec.drain();

  ok(cache.size() == 5, "cache should be full, but has %d", cache.size());

  folly::collectAll(futures.begin(), futures.end())
      .thenTry(
          [](folly::Try<std::vector<folly::Try<std::shared_ptr<const Node>>>>&&
                 result) {
            for (auto& r : result.value()) {
              ok(r.value()->result().hasException(), "should be an error node");
            }
          })
      .wait();

  ok(cache.size() == 5,
     "cache should still be full (no excess) but has %d",
     cache.size());

  ok(cache.get(42, now) == nullptr, "we don't have 42 yet");

  // Now if we "sleep" for long enough, we should be able to evict
  // the error nodes and allow the insert to happen.
  ok(cache.get(42, now) == nullptr, "we don't have 42 yet");
  ok(cache.set(42, 42, now + kErrorTTL + std::chrono::milliseconds(1)),
     "inserted");
  ok(cache.get(42, now) != nullptr, "we found 42 in the cache");
  ok(cache.size() == 5,
     "cache should still be full (no excess) but has %d",
     cache.size());
}

int main() {
  plan_tests(53);
  test_basics();
  test_future();
  return exit_status();
}
