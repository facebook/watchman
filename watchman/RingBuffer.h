/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/experimental/LockFreeRingBuffer.h>

namespace watchman {

/**
 * Fixed-size, lock-free ring buffer. Used for low-latency event logging.
 */
template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(uint32_t capacity)
      : ring_{capacity}, lastClear_{ring_.currentHead()} {}

  void clear() {
    lastClear_.store(ring_.currentHead(), std::memory_order_release);
  }

  void write(const T& entry) {
    ring_.write(entry);
  }

  std::vector<T> readAll() const {
    auto lastClear = lastClear_.load(std::memory_order_acquire);

    std::vector<T> entries;

    auto head = ring_.currentHead();
    T entry;
    while (head.moveBackward() && head >= lastClear &&
           ring_.tryRead(entry, head)) {
      entries.push_back(std::move(entry));
    }
    std::reverse(entries.begin(), entries.end());
    return entries;
  }

 private:
  RingBuffer(RingBuffer&&) = delete;
  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(RingBuffer&&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  folly::LockFreeRingBuffer<T> ring_;
  std::atomic<typename folly::LockFreeRingBuffer<T>::Cursor> lastClear_;
};

} // namespace watchman
