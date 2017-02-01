/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "WatchmanClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

#include <folly/init/Init.h>
#include <folly/io/async/EventBaseThread.h>
#include <folly/json.h>
#include <glog/logging.h>

using namespace folly;
using namespace watchman;
using namespace std::chrono;

int main(int argc, char** argv) {
  folly::init(&argc, &argv);

  system("rm -f hit");

  folly::EventBaseThread ebt;
  auto eb = ebt.getEventBase();

  Promise<Unit> errorCallbackTrigger;
  WatchmanClient c(
      eb,
      Optional<std::string>(),
      nullptr,
      [&errorCallbackTrigger](exception_wrapper&) {
        LOG(INFO) << "Expected global error caught";
        errorCallbackTrigger.setValue();
      });
  c.connect().wait();
  LOG(INFO) << "Connected to watchman";
  SCOPE_EXIT {
    c.close(); // must close before the EventBase is terminated
    ebt.stop();
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::atomic_bool hit(false);

  char current_dir[PATH_MAX];
  CHECK(getcwd(current_dir, PATH_MAX)) << "Error getting current dir";
  dynamic query = dynamic::object("fields", dynamic::array("name"))(
      "expression", dynamic::array("name", "hit"));
  auto sub = c.subscribe(
                  query,
                  current_dir,
                  eb,
                  [&](Try<dynamic>&& data) {
                    if ((*data)["is_fresh_instance"].getBool()) {
                      return;
                    } else {
                      if ((*data)["files"][0].getString().find("hit") !=
                          std::string::npos) {
                        LOG(INFO) << "Got hit";
                        std::unique_lock<std::mutex> lock(mutex);
                        hit = true;
                        cv.notify_all();
                      }
                    }
                  })
                 .wait()
                 .value();

  LOG(INFO) << "Triggering subscritpion";
  system("touch hit");
  LOG(INFO) << "Waiting for hit.";
  std::unique_lock<std::mutex> lock(mutex);
  auto now = std::chrono::system_clock::now();
  if (!cv.wait_until(lock, now + seconds(5), [&]() { return (bool)hit; })) {
    LOG(ERROR) << "FAIL: timeout/no hit";
    return 1;
  }
  hit = false;

  LOG(INFO) << "Unsubscribing";
  c.unsubscribe(sub).wait();
  LOG(INFO) << "Trying to falsely trigger subscription";
  system("rm hit");
  /* sleep override */
  sleep(3);
  if (hit) {
    LOG(ERROR) << "FAIL: still got a hit";
    return 1;
  }
  LOG(INFO) << "PASS: didn't see false trigger after 3 seconds";

  LOG(INFO) << "Testing error handling";
  Promise<Unit> subErrorCallbackTrigger;
  c.subscribe(
       query,
       current_dir,
       eb,
       [&](folly::Try<dynamic>&& data) {
         if (data.hasException()) {
           LOG(INFO) << "Expected subcription error caught";
           subErrorCallbackTrigger.setValue();
         }
       })
      .wait()
      .value();
  c.getConnection().forceEOF();
  try {
    errorCallbackTrigger.getFuture().within(seconds(1)).wait().value();
   } catch(TimedOut& e) {
     LOG(ERROR) << "FAIL: did not get callback from global error handler";
     return 1;
   }
   try {
     subErrorCallbackTrigger.getFuture().within(seconds(1)).wait().value();
   } catch(TimedOut& e) {
     LOG(ERROR) << "FAIL: did not get subscription error";
     return 1;
   }
   LOG(INFO) << "PASS: caught expected errors";

  return 0;
}
