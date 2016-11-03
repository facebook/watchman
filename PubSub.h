/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"
#include "watchman_synchronized.h"
#include "watchman_string.h"
#include "thirdparty/jansson/jansson.h"

#include <deque>
#include <vector>

namespace watchman {

class Publisher : public std::enable_shared_from_this<Publisher> {
 public:
  struct Item {
    // copy of nextSerial_ at the time this was created.
    // The item can be released when all subscribers have
    // observed this serial number.
    uint64_t serial;
    json_ref payload;
  };

  // Generic callback that subscribers can register to arrange
  // to be woken up when something is published
  using Notifier = std::function<void()>;

  // Each subscriber is represented by one of these
  class Subscriber : public std::enable_shared_from_this<Subscriber> {
    // The serial of the last Item to be consumed by
    // this subscriber.
    uint64_t serial_;
    // Subscriber keeps the publisher alive so that no Items are lost
    // if the Publisher is released before all of the subscribers.
    std::shared_ptr<Publisher> publisher_;
    // Advising the subscriber that there may be more items available
    Notifier notify_;

   public:
    ~Subscriber();
    Subscriber(std::shared_ptr<Publisher> pub, Notifier notify);

    // Returns the next published item that this subscriber has
    // not yet observed.
    std::shared_ptr<const Item> getNext();

    // Returns all as yet unseen published items for this subscriber.
    // Equivalent to calling getNext in a loop and sticking the results
    // into a vector.
    std::vector<std::shared_ptr<const Item>> getPending();

    inline uint64_t getSerial() const {
      return serial_;
    }

    inline Notifier& getNotify() {
      return notify_;
    }
  };

  // Register a new subscriber.
  // When the Subscriber object is released, the registration is
  // automatically removed.
  std::shared_ptr<Subscriber> subscribe(Notifier notify);

  // Returns true if there are any subscribers.
  // This is racy and intended to be used to gate building a payload
  // if there are no current subscribers.
  bool hasSubscribers() const;

  // Enqueue a new item, but only if there are subscribers.
  // Returns true if the item was queued.
  bool enqueue(json_ref&& payload);

 private:
  struct state {
    // Serial number to use for the next Item
    uint64_t nextSerial{1};
    // The stream of Items
    std::deque<std::shared_ptr<const Item>> items;
    // The subscribers
    std::vector<std::weak_ptr<Subscriber>> subscribers;

    void collectGarbage();
    void enqueue(json_ref&& payload);
  };
  Synchronized<state> state_;

  friend class Subscriber;
};

// Equivalent to calling getPending on up to two Subscriber and
// joining the resultant vectors together.
std::vector<std::shared_ptr<const Publisher::Item>> getPending(
    const std::shared_ptr<Publisher::Subscriber>& sub1,
    const std::shared_ptr<Publisher::Subscriber>& sub2);
}
