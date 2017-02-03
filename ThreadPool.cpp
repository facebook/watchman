/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "ThreadPool.h"
#include "watchman_log.h"

namespace watchman {

ThreadPool& getThreadPool() {
  static ThreadPool pool;
  return pool;
}

ThreadPool::~ThreadPool() {
  stop();
}

void ThreadPool::start(size_t numWorkers, size_t maxItems) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!workers_.empty()) {
    throw std::runtime_error("ThreadPool already started");
  }
  if (stopping_) {
    throw std::runtime_error("Cannot restart a stopped pool");
  }
  maxItems_ = maxItems;

  for (auto i = 0U; i < numWorkers; ++i) {
    workers_.emplace_back([this, i] {
      w_set_thread_name("ThreadPool-%i", i);
      runWorker();
    });
  }
}

void ThreadPool::runWorker() {
  while (true) {
    std::function<void()> task;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }

    task();
  }
}

void ThreadPool::stop(bool join) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();

  if (join) {
    for (auto& worker : workers_) {
      worker.join();
    }
  }
}

void ThreadPool::run(std::function<void()>&& func) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
      throw std::runtime_error("cannot add tasks after pool has stopped");
    }
    if (tasks_.size() + 1 >= maxItems_) {
      throw std::runtime_error("thread pool queue is full");
    }

    tasks_.emplace_back(std::move(func));
  }

  condition_.notify_one();
}
}
