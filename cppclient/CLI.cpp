/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

/*
  This is a test utility for WatchmanConnection. This works a bit like the
  watchman CLI.

  Build with something like:
  $ LDFLAGS=$(pkg-config watchmanclient --libs) \
      CPPFLAGS=$(pkg-config watchmanclient --cflags) \
      make CLI

  WatchmanConnection is automatically tested via the cppclient test more
  thoroughly.
*/

#include <watchman/WatchmanConnection.h>

#include <iostream>
#include <memory>

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/json.h>

using namespace folly;
using namespace watchman;

int main(int argc, char** argv) {
  folly::init(&argc, &argv);

  folly::ScopedEventBaseThread sebt;
  auto eb = sebt.getEventBase();

  folly::dynamic cmd = folly::dynamic::array;
  for (int i = 1; i < argc; i++) {
    cmd.push_back(std::string(argv[i]));
  }

  auto c = std::make_shared<WatchmanConnection>(eb);
  c->connect()
      .then([&](folly::dynamic version) {
        std::cout << "Connected to watchman: " << version << std::endl;
        std::cout << "Going to run " << cmd << std::endl;
        return c->run(cmd);
      })
      .then([](folly::dynamic result) { LOG(INFO) << "Result: " << result; })
      .onError([](const folly::exception_wrapper& ex) {
        std::cerr << "Failed: " << ex.what() << std::endl;
      })
      .wait();

  c->run(folly::dynamic::array("watch-list"))
      .then([](folly::dynamic res) { std::cout << res << std::endl; })
      .wait();
  c->close();

  return 0;
}
