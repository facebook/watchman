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

#include "watchman/watcher/WatcherRegistry.h"
#include <folly/Conv.h>
#include "watchman/CommandRegistry.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_root.h"

using namespace watchman;

WatcherRegistry::WatcherRegistry(
    const std::string& name,
    std::function<std::shared_ptr<watchman::QueryableView>(watchman_root*)>
        init,
    int priority)
    : name_(name), init_(init), pri_(priority) {
  registerFactory(*this);
}

std::unordered_map<std::string, WatcherRegistry>&
WatcherRegistry::getRegistry() {
  // Meyers singleton
  static std::unordered_map<std::string, WatcherRegistry> registry;
  return registry;
}

void WatcherRegistry::registerFactory(const WatcherRegistry& factory) {
  auto& reg = getRegistry();
  reg.emplace(factory.name_, factory);

  auto capname = folly::to<std::string>("watcher-", factory.name_);
  capability_register(capname.c_str());
}

const WatcherRegistry* WatcherRegistry::getWatcherByName(
    const std::string& name) {
  auto& reg = getRegistry();
  const auto& it = reg.find(name);
  if (it == reg.end()) {
    return nullptr;
  }
  return &it->second;
}

// Helper to DRY in the two success paths in the function below
static inline std::shared_ptr<watchman::QueryableView> reportWatcher(
    const std::string& watcherName,
    watchman_root* root,
    std::shared_ptr<watchman::QueryableView>&& watcher) {
  if (!watcher) {
    throw std::runtime_error(folly::to<std::string>(
        "watcher ",
        watcherName,
        " returned nullptr, but should throw an exception"
        " to correctly report initialization issues"));
  }
  watchman::log(
      watchman::ERR,
      "root ",
      root->root_path,
      " using watcher mechanism ",
      watcher->getName(),
      " (",
      watcherName,
      " was requested)\n");
  return std::move(watcher);
}

std::shared_ptr<watchman::QueryableView> WatcherRegistry::initWatcher(
    watchman_root* root) {
  std::string failureReasons;
  std::string watcherName = root->config.getString("watcher", "auto");

  if (watcherName != "auto") {
    // If they asked for a specific one, let's try to find it
    auto watcher = getWatcherByName(watcherName);

    if (!watcher) {
      failureReasons.append(
          std::string("no watcher named ") + watcherName + std::string(". "));
    } else {
      try {
        return reportWatcher(watcherName, root, watcher->init_(root));
      } catch (const std::exception& e) {
        failureReasons.append(
            watcherName + std::string(": ") + e.what() + std::string(". "));
      }
    }
  }

  // If we get here, let's do auto-selection; build up a list of the
  // watchers that we didn't try already...
  std::vector<const WatcherRegistry*> watchers;
  for (const auto& it : getRegistry()) {
    if (it.first != watcherName) {
      watchers.emplace_back(&it.second);
    }
  }

  // ... and sort with the highest priority first
  std::sort(
      watchers.begin(),
      watchers.end(),
      [](const WatcherRegistry* a, const WatcherRegistry* b) {
        return a->pri_ > b->pri_;
      });

  // and then work through them, taking the first one that sticks
  for (auto& watcher : watchers) {
    try {
      watchman::log(
          watchman::DBG,
          "attempting to use watcher ",
          watcher->getName(),
          " on ",
          root->root_path,
          "\n");
      return reportWatcher(watcherName, root, watcher->init_(root));
    } catch (const watchman::TerminalWatcherError& e) {
      failureReasons.append(
          watcher->getName() + std::string(": ") + e.what() +
          std::string(". "));
      // Don't continue our attempt to use other registered watchers
      // in this case
      break;
    } catch (const std::exception& e) {
      watchman::log(
          watchman::ERR,
          "failed to use watcher ",
          watcher->getName(),
          ": ",
          e.what(),
          ".\n");
      failureReasons.append(
          watcher->getName() + std::string(": ") + e.what() +
          std::string(". "));
    }
  }

  // Nothing worked, report the errors
  throw std::runtime_error(failureReasons);
}

/* vim:ts=2:sw=2:et:
 */
