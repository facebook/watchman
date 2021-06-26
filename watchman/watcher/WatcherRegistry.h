/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

struct watchman_root;

namespace watchman {

class InMemoryView;
class QueryableView;

/**
 * Maintains the list of available watchers.
 * This is fundamentally a map of name -> factory function.
 * Some watchers (kqueue, inotify) are available on multiple operating
 * systems: kqueue on OSX and *BSD, inotify on Linux and Solaris.
 * There are cases where a given watcher is not the preferred mechanism
 * (eg: inotify is implemented in terms of portfs on Solaris, so we
 * prefer to target the portfs layer directly), so we have a concept
 * of priority associated with the watcher.
 * Larger numbers are higher priority and will be favored when performing
 * auto-detection.
 **/
class WatcherRegistry {
 public:
  WatcherRegistry(
      const std::string& name,
      std::function<std::shared_ptr<watchman::QueryableView>(watchman_root*)>
          init,
      int priority = 0);

  /** Locate the appropriate watcher for root and initialize it */
  static std::shared_ptr<watchman::QueryableView> initWatcher(
      watchman_root* root);

  const std::string& getName() const {
    return name_;
  }

 private:
  std::string name_;
  std::function<std::shared_ptr<watchman::QueryableView>(watchman_root*)> init_;
  int pri_;

  static std::unordered_map<std::string, WatcherRegistry>& getRegistry();
  static void registerFactory(const WatcherRegistry& factory);
  static const WatcherRegistry* getWatcherByName(const std::string& name);
};

/**
 * This template makes it less verbose for the common case of defining
 * a name -> class mapping in the registry.
 */
template <class WATCHER>
class RegisterWatcher : public WatcherRegistry {
 public:
  explicit RegisterWatcher(const std::string& name, int priority = 0)
      : WatcherRegistry(
            name,
            [](watchman_root* root) {
              return std::make_shared<watchman::InMemoryView>(
                  root, std::make_shared<WATCHER>(root));
            },
            priority) {}
};

} // namespace watchman
