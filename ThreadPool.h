/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h" // to avoid system header ordering issue on win32
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "Future.h"

namespace watchman {

// Almost the dumbest possible thread pool implementation.
// This allows us to set an upper bound on the number of concurrent
// tasks that are executed in the thread pool.  Contrast with
// std::async which leaves it to the implementation to decide
// whether each async invocation spawns a thread or uses a
// thread pool with an unspecified number of threads.
// Constraining the concurrency is important for watchman so
// that we can limit the amount of I/O that we might induce.

class ThreadPool : public Executor {
 public:
  ThreadPool() = default;
  ~ThreadPool() override;

  // Start a thread pool with the specified number of worker threads
  // and the specified upper bound on the number of queued jobs.
  // The queue limit is intended as a brake in case the system
  // is under a heavy backlog, and can also help surface issues
  // where there a task executing in the pool is blocking on
  // the results of some other task also running in the thread
  // pool.
  void start(size_t numWorkers, size_t maxItems);

  // Request that the worker threads terminate.
  // If `join` is true, wait for the worker threads to terminate.
  void stop(bool join = true);

  // Run a function in the thread pool.
  // This queues up the function for asynchronous execution and
  // may return before func has been executed.
  // If the thread pool has been stopped, throws a runtime_error.
  void run(std::function<void()>&& func) override;

 private:
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;

  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_{false};
  size_t maxItems_;

  void runWorker();
};

// Return a reference to the shared thread pool for the watchman process.
ThreadPool& getThreadPool();
}
