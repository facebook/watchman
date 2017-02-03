/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman_system.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <thread>
#include "Future.h"
#include "ThreadPool.h"
#include "thirdparty/tap.h"

using namespace watchman;

void test_promise() {
  Promise<bool> p;

  auto f = p.getFuture();
  try {
    p.getFuture();
    ok(false, "should throw");
  } catch (const std::logic_error& exc) {
    ok(!strcmp(exc.what(), "Future already obtained"), "can't getFuture twice");
  }
  ok(!f.isReady(), "not yet ready");

  p.setValue(true);
  try {
    p.setValue(false);
    ok(false, "should throw");
  } catch (const std::logic_error& exc) {
    ok(!strcmp(exc.what(), "Promise already fulfilled"),
       "can't setValue twice");
  }

  ok(f.isReady(), "now ready");
  ok(f.get() == true, "got our true value");

  Promise<std::string> s;
  s.setException(std::make_exception_ptr(std::runtime_error("boo")));
  auto f2 = s.getFuture();
  ok(f2.result().hasError(), "holds an error");
  try {
    f2.get();
  } catch (const std::runtime_error& exc) {
    ok(!strcmp(exc.what(), "boo"), "has boo string");
  }
}

void test_thread() {
  Promise<std::string> p;

  std::thread thr([&p] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    p.setValue("done");
  });

  auto f = p.getFuture();
  ok(f.get() == "done", "done in thread");
  thr.join();
}

void test_then() {
  Promise<std::string> p;
  auto f = p.getFuture().then([](Result<std::string>&& result) {
    ok(result.value() == "noice", "got the value we wanted");
    return true;
  });

  p.setValue("noice");

  ok(f.get() == true, "resolved to a boolean future result");

  Promise<std::string> p2;
  auto f1 = p2.getFuture();
  p2.setValue("woot");
  auto f2 = f1.then([](Result<std::string>&& result) {
    auto& str = result.value();
    std::transform(
        str.begin(), str.end(), str.begin(), [](std::string::value_type c) {
          return std::toupper(c);
        });
    return str;
  });
  ok(f2.get() == "WOOT",
     "callback applied after initial promise was fulfilled");

  Promise<std::string> p3;
  std::thread thr([&p3] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    p3.setValue("done");
  });
  auto f3 = p3.getFuture().then(
      [](Result<std::string>&& result) { return result.value().append("!"); });
  ok(f3.get() == "done!", ".then worked across threads");
  thr.join();

  auto f4 = makeFuture<std::string>("foo").then([](
      Result<std::string>&& result) { return makeFuture(std::move(result)); });
  ok(f4.get() == "foo", "unwrapped future in .then chain");
}

void test_collect() {
  std::vector<Future<bool>> futures;
  Promise<bool> p1, p2;

  futures.emplace_back(p1.getFuture());
  futures.emplace_back(p2.getFuture());

  auto f = collectAll(futures.begin(), futures.end());

  ok(!f.isReady(), "none ready yet");
  p1.setValue(true);
  ok(!f.isReady(), "none ready yet");
  p2.setValue(false);
  ok(f.isReady(), "now ready");

  f.then([](Result<std::vector<Result<bool>>>&& result) {
    auto& vec = result.value();
    ok(vec[0].value(), "p1 result was true");
    ok(!vec[1].value(), "p2 result was false");
  });
}

void test_via() {
  ThreadPool pool;
  pool.start(1, 1024);

  Promise<bool> barrier;

  auto f = makeFuture().via(&pool).then([&barrier](Result<Unit>&&) {
    diag("waiting for barrier");
    barrier.getFuture().wait();
    return 42;
  });

  ok(!f.isReady(), "hasn't run in the thread yet");
  barrier.setValue(true);

  ok(f.get() == 42, "came back on the other side");
}

int main() {
  plan_tests(20);
  test_promise();
  test_thread();
  test_then();
  test_collect();
  test_via();
  return exit_status();
}
