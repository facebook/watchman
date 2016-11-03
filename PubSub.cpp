/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "PubSub.h"
#include <algorithm>
#include <iterator>

namespace watchman {

Publisher::Subscriber::Subscriber(
    std::shared_ptr<Publisher> pub,
    Notifier notify)
    : serial_(0), publisher_(std::move(pub)), notify_(notify) {}

Publisher::Subscriber::~Subscriber() {
  auto wlock = publisher_->state_.wlock();
  auto it = wlock->subscribers.begin();
  while (it != wlock->subscribers.end()) {
    auto sub = it->lock();
    // Prune vacated weak_ptr's or those that point to us
    if (!sub || sub.get() == this) {
      it = wlock->subscribers.erase(it);
    } else {
      ++it;
    }
  }
  // Take this opportunity to reap anything that is no longer
  // referenced now that we've removed some subscriber(s)
  wlock->collectGarbage();
}

std::shared_ptr<const Publisher::Item> Publisher::Subscriber::getNext() {
  auto rlock = publisher_->state_.rlock();
  for (auto& item : rlock->items) {
    if (item->serial > serial_) {
      serial_ = item->serial;
      return item;
    }
  }

  return nullptr;
}

std::vector<std::shared_ptr<const Publisher::Item>>
Publisher::Subscriber::getPending() {
  std::vector<std::shared_ptr<const Publisher::Item>> items;

  auto rlock = publisher_->state_.rlock();
  for (auto& item : rlock->items) {
    if (item->serial > serial_) {
      serial_ = item->serial;
      items.push_back(item);
    }
  }

  return items;
}

template <typename Vec>
void moveVec(Vec& dest, Vec&& src) {
  std::move(src.begin(), src.end(), std::back_inserter(dest));
}

std::vector<std::shared_ptr<const Publisher::Item>> getPending(
    const std::shared_ptr<Publisher::Subscriber>& sub1,
    const std::shared_ptr<Publisher::Subscriber>& sub2) {
  std::vector<std::shared_ptr<const Publisher::Item>> items;

  if (sub1) {
    moveVec(items, sub1->getPending());
  }
  if (sub2) {
    moveVec(items, sub2->getPending());
  }

  return items;
}

std::shared_ptr<Publisher::Subscriber> Publisher::subscribe(Notifier notify) {
  auto sub =
      std::make_shared<Publisher::Subscriber>(shared_from_this(), notify);
  state_.wlock()->subscribers.emplace_back(sub);
  return sub;
}

bool Publisher::hasSubscribers() const {
  return !state_.rlock()->subscribers.empty();
}

void Publisher::state::collectGarbage() {
  if (items.empty()) {
    return;
  }

  uint64_t minSerial = std::numeric_limits<uint64_t>::max();
  for (auto& it : subscribers) {
    auto sub = it.lock();
    if (sub) {
      minSerial = std::min(minSerial, sub->getSerial());
    }
  }

  while (!items.empty() && items.front()->serial < minSerial) {
    items.pop_front();
  }
}

bool Publisher::enqueue(json_ref&& payload) {
  std::vector<std::shared_ptr<Subscriber>> subscribers;

  {
    auto wlock = state_.wlock();

    // We need to collect live references for the notify portion,
    // but since we're holding the wlock, take this opportunity to
    // detect and prune dead subscribers and clean up some garbage.
    auto it = wlock->subscribers.begin();
    while (it != wlock->subscribers.end()) {
      auto sub = it->lock();
      // Prune vacated weak_ptr's
      if (!sub) {
        it = wlock->subscribers.erase(it);
      } else {
        ++it;
        // Remember that live reference so that we can notify
        // outside of the lock below.
        subscribers.emplace_back(std::move(sub));
      }
    }

    wlock->collectGarbage();

    if (subscribers.empty()) {
      return false;
    }

    auto item = std::make_shared<Item>();
    item->payload = std::move(payload);
    item->serial = wlock->nextSerial++;
    wlock->items.emplace_back(std::move(item));
  }

  // and notify them outside of the lock
  for (auto& sub : subscribers) {
    auto& n = sub->getNotify();
    if (n) {
      n();
    }
  }
  return true;
}
}
