/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "PubSub.h"
#include <algorithm>
#include <iterator>

namespace watchman {

Publisher::Subscriber::Subscriber(
    std::shared_ptr<Publisher> pub,
    Notifier notify,
    const w_string& info)
    : serial_(0),
      publisher_(std::move(pub)),
      notify_(notify),
      info_(std::move(info)) {}

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

void Publisher::Subscriber::getPending(
    std::vector<std::shared_ptr<const Item>>& pending) {
  {
    auto rlock = publisher_->state_.rlock();
    auto& items = rlock->items;

    if (items.empty()) {
      return;
    }

    // First we walk back to find the end of the range that
    // we have seen previously.
    int firstIndex;
    for (firstIndex = int(items.size()) - 1; firstIndex >= 0; --firstIndex) {
      if (items[firstIndex]->serial <= serial_) {
        break;
      }
    }

    // We found the item before the one we really want, so
    // increment the index; we'll copy the remaining items.
    ++firstIndex;
    bool updated = false;

    while (firstIndex < int(items.size())) {
      pending.push_back(items[firstIndex]);
      ++firstIndex;
      updated = true;
    }

    if (updated) {
      serial_ = pending.back()->serial;
    }

    return;
  }
}

void getPending(
    std::vector<std::shared_ptr<const Publisher::Item>>& items,
    const std::shared_ptr<Publisher::Subscriber>& sub1,
    const std::shared_ptr<Publisher::Subscriber>& sub2) {
  if (sub1) {
    sub1->getPending(items);
  }
  if (sub2) {
    sub2->getPending(items);
  }
}

std::shared_ptr<Publisher::Subscriber> Publisher::subscribe(
    Notifier notify,
    const w_string& info) {
  auto sub =
      std::make_shared<Publisher::Subscriber>(shared_from_this(), notify, info);
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

json_ref Publisher::getDebugInfo() const {
  auto ret = json_object();

  auto rlock = state_.rlock();
  ret.set("next_serial", json_integer(rlock->nextSerial));

  auto subscribers = json_array();
  auto& subscribers_arr = subscribers.array();

  for (auto& sub_ref : rlock->subscribers) {
    auto sub = sub_ref.lock();
    if (sub) {
      auto sub_json = json_object({{"serial", json_integer(sub->getSerial())},
                                   {"info", w_string_to_json(sub->getInfo())}});
      subscribers_arr.emplace_back(sub_json);
    } else {
      // This is a subscriber that is now dead. It will be cleaned up the next
      // time enqueue is called.
    }
  }

  ret.set("subscribers", std::move(subscribers));

  auto items = json_array();
  auto& items_arr = items.array();

  for (auto& item : rlock->items) {
    auto item_json = json_object(
        {{"serial", json_integer(item->serial)}, {"payload", item->payload}});
    items_arr.emplace_back(item_json);
  }

  ret.set("items", std::move(items));

  return ret;
}
}
